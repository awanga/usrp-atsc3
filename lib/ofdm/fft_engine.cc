// fft_engine.cc — FFT Engine implementation
//
// FFTW3 backend for float mode, Cooley-Tukey for fixed-point mode

#include "fft_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifndef ATSC3_FIXED_POINT
#include <fftw3.h>
#endif

namespace atsc3 {
namespace ofdm {

namespace {

// Validate FFT size
bool is_valid_fft_size(size_t n) {
    return n == 4096 || n == 8192 || n == 16384 || n == 32768;
}

}  // namespace

#ifndef ATSC3_FIXED_POINT

//==============================================================================
// FFTW3 Backend (float mode)
//==============================================================================

class FftwEngine : public FftEngine {
public:
    FftwEngine(const FftConfig& config)
        : size_(static_cast<size_t>(config.size)),
          direction_(config.direction),
          normalize_inverse_(config.normalize_inverse) {
        if (!is_valid_fft_size(size_)) {
            throw std::invalid_argument("Invalid FFT size");
        }

        // Allocate aligned buffers for FFTW
        in_buf_ = fftwf_alloc_complex(size_);
        out_buf_ = fftwf_alloc_complex(size_);

        if (!in_buf_ || !out_buf_) {
            throw std::runtime_error("Failed to allocate FFT buffers");
        }

        // Load wisdom if available
        if (!config.wisdom_path.empty()) {
            load_fftw_wisdom(config.wisdom_path);
        }

        // Create plan
        unsigned flags = FFTW_ESTIMATE;
        if (config.use_patient_plan) {
            flags = FFTW_PATIENT;
        }

        int sign = (direction_ == FftDirection::kForward) ? FFTW_FORWARD : FFTW_BACKWARD;

        plan_ = fftwf_plan_dft_1d(static_cast<int>(size_), in_buf_, out_buf_, sign, flags);

        if (!plan_) {
            fftwf_free(in_buf_);
            fftwf_free(out_buf_);
            throw std::runtime_error("Failed to create FFT plan");
        }

        // Save wisdom after planning
        if (!config.wisdom_path.empty()) {
            save_fftw_wisdom(config.wisdom_path);
        }
    }

    ~FftwEngine() override {
        if (plan_) {
            fftwf_destroy_plan(plan_);
        }
        if (in_buf_) {
            fftwf_free(in_buf_);
        }
        if (out_buf_) {
            fftwf_free(out_buf_);
        }
    }

    void process(const sample_t* in, sample_t* out) override {
        // sample_t is complex<float> in float mode
        // Copy input to aligned buffer (element-wise to avoid memcpy on non-trivial type)
        for (size_t i = 0; i < size_; ++i) {
            in_buf_[i][0] = in[i].real();
            in_buf_[i][1] = in[i].imag();
        }

        // Execute FFT
        fftwf_execute(plan_);

        // Copy output and optionally normalize
        if (normalize_inverse_ && direction_ == FftDirection::kInverse) {
            float scale = 1.0f / static_cast<float>(size_);
            for (size_t i = 0; i < size_; ++i) {
                out[i] = sample_t(out_buf_[i][0] * scale, out_buf_[i][1] * scale);
            }
        } else {
            for (size_t i = 0; i < size_; ++i) {
                out[i] = sample_t(out_buf_[i][0], out_buf_[i][1]);
            }
        }
    }

    size_t get_size() const override { return size_; }
    FftDirection get_direction() const override { return direction_; }
    bool is_fftw_backend() const override { return true; }

private:
    size_t size_;
    FftDirection direction_;
    bool normalize_inverse_;

    fftwf_complex* in_buf_ = nullptr;
    fftwf_complex* out_buf_ = nullptr;
    fftwf_plan plan_ = nullptr;
};

std::unique_ptr<FftEngine> FftEngine::create(const FftConfig& config) {
    return std::make_unique<FftwEngine>(config);
}

bool load_fftw_wisdom(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    return fftwf_import_wisdom_from_filename(path.c_str()) != 0;
}

bool save_fftw_wisdom(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    return fftwf_export_wisdom_to_filename(path.c_str()) != 0;
}

#else  // ATSC3_FIXED_POINT

//==============================================================================
// Fixed-Point Backend (Cooley-Tukey)
//==============================================================================

class FixedPointFftEngine : public FftEngine {
public:
    FixedPointFftEngine(const FftConfig& config)
        : size_(static_cast<size_t>(config.size)),
          direction_(config.direction),
          normalize_inverse_(config.normalize_inverse) {
        if (!is_valid_fft_size(size_)) {
            throw std::invalid_argument("Invalid FFT size");
        }

        // Precompute twiddle factors
        compute_twiddles();

        // Precompute bit-reversal indices
        compute_bit_reversal();
    }

    ~FixedPointFftEngine() override = default;

    void process(const sample_t* in, sample_t* out) override {
        // sample_t is complex<int16_t> in fixed-point mode
        // Copy input with bit-reversal
        for (size_t i = 0; i < size_; ++i) {
            work_buf_[bit_rev_[i]] = in[i];
        }

        // Cooley-Tukey radix-2 DIT
        cooley_tukey_dit();

        // Copy to output with optional normalization
        if (normalize_inverse_ && direction_ == FftDirection::kInverse) {
            // Normalize by 1/N using right shift (approximate for power-of-2)
            int shift = log2_size();
            for (size_t i = 0; i < size_; ++i) {
                int16_t re = static_cast<int16_t>(work_buf_[i].real() >> shift);
                int16_t im = static_cast<int16_t>(work_buf_[i].imag() >> shift);
                out[i] = sample_t(re, im);
            }
        } else {
            std::copy(work_buf_.begin(), work_buf_.end(), out);
        }
    }

    void process(const std::complex<float>* in, std::complex<float>* out) override {
        // Convert float to fixed-point, process, convert back
        std::vector<sample_t> in_fixed(size_);
        std::vector<sample_t> out_fixed(size_);

        for (size_t i = 0; i < size_; ++i) {
            in_fixed[i] = sample_t(float_to_q15(in[i].real()), float_to_q15(in[i].imag()));
        }

        process(in_fixed.data(), out_fixed.data());

        for (size_t i = 0; i < size_; ++i) {
            out[i] = std::complex<float>(q15_to_float(out_fixed[i].real()),
                                         q15_to_float(out_fixed[i].imag()));
        }
    }

    size_t get_size() const override { return size_; }
    FftDirection get_direction() const override { return direction_; }
    bool is_fftw_backend() const override { return false; }

private:
    void compute_twiddles() {
        twiddles_.resize(size_ / 2);
        double sign = (direction_ == FftDirection::kForward) ? -1.0 : 1.0;

        for (size_t k = 0; k < size_ / 2; ++k) {
            double angle = sign * 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(size_);
            // Store as Q1.15 fixed-point
            twiddles_[k] = sample_t(float_to_q15(static_cast<float>(std::cos(angle))),
                                    float_to_q15(static_cast<float>(std::sin(angle))));
        }

        work_buf_.resize(size_);
    }

    void compute_bit_reversal() {
        bit_rev_.resize(size_);
        int bits = log2_size();

        for (size_t i = 0; i < size_; ++i) {
            size_t rev = 0;
            size_t val = i;
            for (int b = 0; b < bits; ++b) {
                rev = (rev << 1) | (val & 1);
                val >>= 1;
            }
            bit_rev_[i] = rev;
        }
    }

    int log2_size() const {
        int bits = 0;
        size_t n = size_;
        while (n > 1) {
            n >>= 1;
            ++bits;
        }
        return bits;
    }

    void cooley_tukey_dit() {
        // Radix-2 decimation-in-time FFT
        for (size_t stage = 1; stage <= static_cast<size_t>(log2_size()); ++stage) {
            size_t m = 1u << stage;       // Butterfly span
            size_t m2 = m >> 1;           // Half span
            size_t twiddle_step = size_ / m;

            for (size_t k = 0; k < size_; k += m) {
                for (size_t j = 0; j < m2; ++j) {
                    size_t idx_even = k + j;
                    size_t idx_odd = k + j + m2;

                    // Get twiddle factor W_N^j
                    sample_t w = twiddles_[j * twiddle_step];

                    // Complex multiply: t = w * odd
                    // Using Q15 fixed-point multiplication
                    sample_t odd = work_buf_[idx_odd];
                    int32_t t_re = (static_cast<int32_t>(w.real()) * odd.real() -
                                    static_cast<int32_t>(w.imag()) * odd.imag()) >>
                                   ATSC3_Q_FRACTIONAL_BITS;
                    int32_t t_im = (static_cast<int32_t>(w.real()) * odd.imag() +
                                    static_cast<int32_t>(w.imag()) * odd.real()) >>
                                   ATSC3_Q_FRACTIONAL_BITS;

                    sample_t t(static_cast<int16_t>(t_re), static_cast<int16_t>(t_im));

                    // Butterfly
                    sample_t even = work_buf_[idx_even];
                    work_buf_[idx_even] =
                        sample_t(even.real() + t.real(), even.imag() + t.imag());
                    work_buf_[idx_odd] =
                        sample_t(even.real() - t.real(), even.imag() - t.imag());
                }
            }
        }
    }

    size_t size_;
    FftDirection direction_;
    bool normalize_inverse_;

    std::vector<sample_t> twiddles_;
    std::vector<sample_t> work_buf_;
    std::vector<size_t> bit_rev_;
};

std::unique_ptr<FftEngine> FftEngine::create(const FftConfig& config) {
    return std::make_unique<FixedPointFftEngine>(config);
}

bool load_fftw_wisdom(const std::string& /* path */) {
    // No FFTW in fixed-point mode
    return false;
}

bool save_fftw_wisdom(const std::string& /* path */) {
    // No FFTW in fixed-point mode
    return false;
}

#endif  // ATSC3_FIXED_POINT

}  // namespace ofdm
}  // namespace atsc3
