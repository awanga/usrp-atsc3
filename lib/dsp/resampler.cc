// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// resampler.cc — rational polyphase resampler (windowed sinc, exact
// position accumulator)

#include "resampler.h"

#include <cmath>
#include <numeric>
#include <stdexcept>

namespace atsc3 {
namespace dsp {

namespace {

// Zeroth-order modified Bessel function of the first kind (series).
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) {
            break;
        }
    }
    return sum;
}

// Kaiser's beta for a stopband attenuation in dB (Kaiser 1974).
double kaiser_beta(double atten_db) {
    if (atten_db > 50.0) {
        return 0.1102 * (atten_db - 8.7);
    }
    if (atten_db > 21.0) {
        return 0.5842 * std::pow(atten_db - 21.0, 0.4) + 0.07886 * (atten_db - 21.0);
    }
    return 0.0;
}

#ifdef ATSC3_FIXED_POINT
int16_t round_sat_q15(int64_t acc) {
    // Unconditional bias then arithmetic shift: round half up.
    int64_t r = (acc + (int64_t{1} << 14)) >> 15;
    if (r > 32767) {
        return 32767;
    }
    if (r < -32767) {
        return -32767;
    }
    return static_cast<int16_t>(r);
}
#endif

}  // namespace

Resampler::Resampler(const ResamplerConfig& config) : config_(config) {
    const size_t taps = config_.taps_per_phase;
    if (config_.input_rate_hz == 0 || config_.output_rate_hz == 0 || taps == 0 || taps % 2 != 0 ||
        config_.num_phases == 0 || !(config_.passband_hz < config_.stopband_hz)) {
        throw std::invalid_argument("Resampler: invalid configuration");
    }
    uint64_t g = std::gcd(config_.input_rate_hz, config_.output_rate_hz);
    l_ = config_.output_rate_hz / g;
    m_ = config_.input_rate_hz / g;

    // Lowpass at the input rate: 6 dB point fc (cycles per input sample),
    // h(a) = 2 fc sinc(2 fc a) w(a) for a position offset a in samples.
    // Designed once in double, quantized for storage.
    const double fc = 0.5 * (config_.passband_hz + config_.stopband_hz) /
                      static_cast<double>(config_.input_rate_hz);
    const double beta = kaiser_beta(config_.stopband_db);
    const double half = static_cast<double>(taps) / 2.0;
    coeffs_.assign(config_.num_phases, std::vector<Coeff>(taps));
    std::vector<double> h(taps);
    for (size_t p = 0; p < config_.num_phases; ++p) {
        const double mu = static_cast<double>(p) / static_cast<double>(config_.num_phases);
        double sum = 0.0;
        for (size_t j = 0; j < taps; ++j) {
            // Tap j weights input floor(x) - taps/2 + 1 + j; offset x - n.
            double a = mu + half - 1.0 - static_cast<double>(j);
            double u = 2.0 * fc * a;
            double sinc = (u == 0.0) ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
            double r = a / half;
            double w = (std::abs(r) >= 1.0)
                           ? 0.0
                           : bessel_i0(beta * std::sqrt(1.0 - r * r)) / bessel_i0(beta);
            h[j] = 2.0 * fc * sinc * w;
            sum += h[j];
        }
#ifdef ATSC3_FIXED_POINT
        // Quantize to a DC gain of 32767/32768 (-0.0003 dB): unit gain is
        // not representable when a branch is a single tap of 1.0, which
        // happens for branch 0 when the cutoff sits at the input Nyquist
        // (the default). The largest tap absorbs the rounding residue.
        constexpr int32_t kGain = 32767;
        int32_t qsum = 0;
        size_t peak = 0;
        for (size_t j = 0; j < taps; ++j) {
            double v = std::round(h[j] / sum * kGain);
            v = std::fmax(-32767.0, std::fmin(32767.0, v));
            coeffs_[p][j] = static_cast<int16_t>(v);
            qsum += coeffs_[p][j];
            if (std::abs(h[j]) > std::abs(h[peak])) {
                peak = j;
            }
        }
        int32_t adjusted = coeffs_[p][peak] + (kGain - qsum);
        if (adjusted > 32767 || adjusted < -32767) {
            throw std::invalid_argument("Resampler: filter does not fit Q1.15");
        }
        coeffs_[p][peak] = static_cast<int16_t>(adjusted);
#else
        for (size_t j = 0; j < taps; ++j) {
            coeffs_[p][j] = static_cast<float>(h[j] / sum);
        }
#endif
    }
    ring_.assign(config_.history + taps, sample_t{});
}

void Resampler::reset() {
    n_in_ = 0;
    pos_index_ = 0;
    pos_frac_ = 0;
    std::fill(ring_.begin(), ring_.end(), sample_t{});
}

sample_t Resampler::input_at(uint64_t i) const {
    return ring_[i % ring_.size()];
}

sample_t Resampler::interpolate(uint64_t index, size_t phase) const {
    const size_t taps = config_.taps_per_phase;
    const std::vector<Coeff>& c = coeffs_[phase];
    // Inputs before sample 0 are zero.
    const int64_t first = static_cast<int64_t>(index) - static_cast<int64_t>(taps / 2) + 1;
#ifdef ATSC3_FIXED_POINT
    int64_t re = 0;
    int64_t im = 0;
    for (size_t j = 0; j < taps; ++j) {
        int64_t n = first + static_cast<int64_t>(j);
        if (n < 0) {
            continue;
        }
        sample_t x = input_at(static_cast<uint64_t>(n));
        re += int64_t{c[j]} * x.real();
        im += int64_t{c[j]} * x.imag();
    }
    return sample_t(round_sat_q15(re), round_sat_q15(im));
#else
    float re = 0.0f;
    float im = 0.0f;
    for (size_t j = 0; j < taps; ++j) {
        int64_t n = first + static_cast<int64_t>(j);
        if (n < 0) {
            continue;
        }
        sample_t x = input_at(static_cast<uint64_t>(n));
        re += c[j] * x.real();
        im += c[j] * x.imag();
    }
    return sample_t(re, im);
#endif
}

size_t Resampler::process(const sample_t* in, size_t n, sample_t* out, size_t max_out) {
    const uint64_t half = config_.taps_per_phase / 2;
    const uint64_t phases = config_.num_phases;
    size_t produced = 0;
    size_t consumed = 0;
    for (;;) {
        // Nearest branch to the fractional position; rounding up to a
        // whole phase count moves to the next input sample.
        uint64_t phase = (2 * pos_frac_ * phases + l_) / (2 * l_);
        uint64_t index = pos_index_;
        if (phase == phases) {
            phase = 0;
            ++index;
        }
        if (index + half < n_in_ && produced < max_out) {
            if (n_in_ > ring_.size() && index + 1 < n_in_ - ring_.size() + half) {
                // Pending outputs fell out of the history: the caller
                // gave too small an output buffer for too long.
                throw std::length_error("Resampler: output backlog exceeded the history");
            }
            out[produced++] = interpolate(index, static_cast<size_t>(phase));
            pos_frac_ += m_;
            pos_index_ += pos_frac_ / l_;
            pos_frac_ %= l_;
            continue;
        }
        if (consumed == n) {
            break;
        }
        ring_[n_in_ % ring_.size()] = in[consumed++];
        ++n_in_;
    }
    return produced;
}

bool Resampler::set_position(const ResamplerPosition& p) {
    if (p.den == 0) {
        return false;
    }
    uint64_t index = p.index + p.frac / p.den;
    uint64_t frac = ((p.frac % p.den) * l_ * 2 + p.den) / (2 * p.den);
    if (frac == l_) {
        frac = 0;
        ++index;
    }
    const uint64_t half = config_.taps_per_phase / 2;
    // The oldest input the output needs, index - half + 1, must still be
    // in the ring (same test as in process()).
    if (n_in_ > ring_.size() && index + 1 < n_in_ - ring_.size() + half) {
        return false;
    }
    pos_index_ = index;
    pos_frac_ = frac;
    return true;
}

ResamplerPosition Resampler::next_position() const {
    return ResamplerPosition{pos_index_, pos_frac_, l_};
}

ResamplerPosition Resampler::position_of_output(uint64_t k) const {
    uint64_t t = k * m_;
    return ResamplerPosition{t / l_, t % l_, l_};
}

}  // namespace dsp
}  // namespace atsc3
