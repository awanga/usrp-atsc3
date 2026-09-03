// timing_recovery.cc — Symbol timing recovery implementation
//
// Gardner TED + polyphase interpolator for OFDM symbol timing

#include "timing_recovery.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace sync {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Sinc function with proper limit at x=0
inline double sinc(double x) {
    if (std::abs(x) < 1e-10) {
        return 1.0;
    }
    return std::sin(kPi * x) / (kPi * x);
}

// Raised cosine window for filter design
inline double raised_cosine(double t, double rolloff, double T) {
    if (rolloff < 1e-10) {
        return sinc(t / T);
    }

    double denom = 1.0 - 4.0 * rolloff * rolloff * t * t / (T * T);
    if (std::abs(denom) < 1e-10) {
        // At singularity points
        return (kPi / (4.0 * T)) * sinc(1.0 / (2.0 * rolloff));
    }

    return sinc(t / T) * std::cos(kPi * rolloff * t / T) / denom;
}

#ifdef ATSC3_FIXED_POINT
// Round-to-nearest Q1.15 quantizer for the one-time filter coefficient
// table (a ROM in RTL terms, computed offline either way). Deliberately
// not float_to_q15(): that helper truncates toward zero, which is the
// right convention for the per-sample runtime datapath (matches actual
// RTL arithmetic and lib/types.h's documented behavior elsewhere), but
// applied independently to 32 filter taps it's a systematic (not just
// random) downward bias -- every tap's magnitude gets rounded down, not
// just quantized. A raised-cosine/Kaiser-windowed filter has many
// small-magnitude taps, where that bias is proportionally largest.
// Rounding instead of truncating for this one-time table closed a real
// gap: the interpolator's fixed-point-vs-reference SNR was ~32 dB with
// truncation, comfortably >40 dB with rounding, and this is exactly what
// a real coefficient-ROM generator would do too.
int16_t round_to_q15(double x) {
    long v = std::lround(x * 32768.0);
    if (v > 32767) {
        v = 32767;
    }
    if (v < -32768) {
        v = -32768;
    }
    return static_cast<int16_t>(v);
}

int16_t saturate_i16(int64_t v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32767) {
        return -32767;
    }
    return static_cast<int16_t>(v);
}
#endif

}  // namespace

//==============================================================================
// PolyphaseInterpolator Implementation
//==============================================================================

PolyphaseInterpolator::PolyphaseInterpolator(const PolyphaseConfig& config) : config_(config) {
    if (config_.num_phases == 0 || config_.taps_per_phase == 0) {
        throw std::invalid_argument("Invalid polyphase config: phases and taps must be > 0");
    }
    init_coefficients();
}

PolyphaseInterpolator::~PolyphaseInterpolator() = default;

void PolyphaseInterpolator::init_coefficients() {
    // Design a polyphase filter bank using windowed sinc
    // Total filter length = num_phases * taps_per_phase
    size_t total_taps = config_.num_phases * config_.taps_per_phase;

    // Design prototype filter (interpolation filter). This is a one-time
    // filter design at construction/reconfiguration time, not a
    // per-sample RTL-mapped computation -- double here is the same kind
    // of "designed once, quantized for storage" step as the CORDIC atan
    // table or the FFT twiddle ROM, not a case of the per-sample datapath
    // hiding double math (that's interpolate(), below, which is genuine
    // integer arithmetic under ATSC3_FIXED_POINT).
    std::vector<double> prototype(total_taps);
    double center = static_cast<double>(total_taps - 1) / 2.0;

    // Rolloff factor for raised cosine (0.2 is typical)
    constexpr double kRolloff = 0.2;

    // Symbol period in samples at interpolated rate
    double T = static_cast<double>(config_.num_phases);

    for (size_t i = 0; i < total_taps; ++i) {
        double t = static_cast<double>(i) - center;
        prototype[i] = raised_cosine(t, kRolloff, T);
    }

    // Apply Kaiser window for better stopband
    double beta = 5.0;  // Kaiser beta (moderate sidelobe suppression)
    for (size_t i = 0; i < total_taps; ++i) {
        double x = 2.0 * static_cast<double>(i) / static_cast<double>(total_taps - 1) - 1.0;
        // Approximate Kaiser window with simpler formula
        double w = std::cosh(beta * std::sqrt(1.0 - x * x)) / std::cosh(beta);
        prototype[i] *= w;
    }

    // Normalize
    double sum = 0.0;
    for (size_t i = 0; i < total_taps; i += config_.num_phases) {
        sum += prototype[i];
    }
    if (std::abs(sum) > 1e-10) {
        for (double& coeff : prototype) {
            coeff /= sum;
        }
    }

    // Split into polyphase components
    // Phase p gets coefficients [p, p + num_phases, p + 2*num_phases, ...]
    coeffs_.resize(config_.num_phases);
    for (size_t p = 0; p < config_.num_phases; ++p) {
        coeffs_[p].resize(config_.taps_per_phase);
        for (size_t t = 0; t < config_.taps_per_phase; ++t) {
            size_t idx = p + t * config_.num_phases;
#ifdef ATSC3_FIXED_POINT
            coeffs_[p][t] = round_to_q15(prototype[idx]);
#else
            coeffs_[p][t] = static_cast<float>(prototype[idx]);
#endif
        }
    }
}

sample_t PolyphaseInterpolator::interpolate(const sample_t* buf, size_t buf_idx, double mu,
                                            size_t buf_size) const {
    // Clamp mu to [0, 1)
    mu = mu - std::floor(mu);

    // Select phase based on fractional delay
    // mu=0 -> phase 0 (no delay), mu approaching 1 -> highest phase
    size_t phase = static_cast<size_t>(mu * config_.num_phases);
    if (phase >= config_.num_phases) {
        phase = config_.num_phases - 1;
    }

    const auto& taps = coeffs_[phase];
    size_t n_taps = config_.taps_per_phase;
    // wrap: the modulus for addressing buf. Defaults to n_taps so callers
    // whose buffer is exactly taps_per_phase long see unchanged behavior;
    // a caller with a larger ring buffer must pass its real size (see the
    // header comment) or buf_idx's high bits are silently discarded.
    size_t wrap = (buf_size == 0) ? n_taps : buf_size;

#ifdef ATSC3_FIXED_POINT
    // Genuine fixed-point FIR dot product (Phase 9.0b rewrite): Q1.15
    // taps times Q1.15 samples, summed as raw (unshifted) products in a
    // wide accumulator, rescaled once at the end -- not per-sample
    // double, which is what this used to do even under
    // ATSC3_FIXED_POINT (taps stayed float and the whole accumulation
    // ran in double).
    int64_t acc_re = 0;
    int64_t acc_im = 0;

    for (size_t t = 0; t < n_taps; ++t) {
        // Index into circular buffer (going backwards)
        size_t idx = (buf_idx + wrap - 1 - t) % wrap;
        acc_re += static_cast<int64_t>(buf[idx].real()) * taps[t];
        acc_im += static_cast<int64_t>(buf[idx].imag()) * taps[t];
    }

    // Round-to-nearest on the final rescale, not truncation: a plain
    // arithmetic >>15 is floor division, a full LSB of systematic bias in
    // one direction (worse for negative accumulator values, which floor
    // *away* from zero relative to round-to-nearest).
    constexpr int64_t kRoundBias = int64_t(1) << 14;
    return sample_t(saturate_i16((acc_re + kRoundBias) >> 15),
                    saturate_i16((acc_im + kRoundBias) >> 15));
#else
    float acc_re = 0.0f;
    float acc_im = 0.0f;

    for (size_t t = 0; t < n_taps; ++t) {
        size_t idx = (buf_idx + wrap - 1 - t) % wrap;
        acc_re += buf[idx].real() * taps[t];
        acc_im += buf[idx].imag() * taps[t];
    }

    return sample_t(acc_re, acc_im);
#endif
}

//==============================================================================
// GardnerTed Implementation
//==============================================================================

GardnerTed::GardnerTed() : prev_symbol_(0, 0), prev_mid_(0, 0), have_prev_(false) {}

GardnerTed::~GardnerTed() = default;

void GardnerTed::reset() {
    prev_symbol_ = sample_t(0, 0);
    prev_mid_ = sample_t(0, 0);
    have_prev_ = false;
}

double GardnerTed::compute_error(sample_t x_curr, sample_t x_mid, sample_t x_prev) const {
    // Gardner TED: e = Re{ (x_curr - x_prev) * conj(x_mid) }
#ifdef ATSC3_FIXED_POINT
    // Genuine integer arithmetic (Phase 9.0b rewrite). diff needs a wider
    // type than int16_t: two Q1.15 values up to +-32767 can differ by up
    // to 65534. The dot product is kept in its raw (unshifted) Q1.15 x
    // Q1.15 scale and only rescaled once at the return boundary --
    // TimingRecovery::update_loop() consumes this same raw scale
    // internally without round-tripping through float_to_q15's saturating
    // [-1, 1) assumption, since this error signal is not itself bounded
    // to that range (see update_loop()'s comment).
    int32_t diff_re = static_cast<int32_t>(x_curr.real()) - static_cast<int32_t>(x_prev.real());
    int32_t diff_im = static_cast<int32_t>(x_curr.imag()) - static_cast<int32_t>(x_prev.imag());

    int64_t error_raw =
        static_cast<int64_t>(diff_re) * x_mid.real() + static_cast<int64_t>(diff_im) * x_mid.imag();
    int64_t error_q15 = error_raw >> 15;
    return static_cast<double>(error_q15) / 32768.0;
#else
    float diff_re = x_curr.real() - x_prev.real();
    float diff_im = x_curr.imag() - x_prev.imag();

    // Real part of (diff * conj(mid))
    return static_cast<double>(diff_re * x_mid.real() + diff_im * x_mid.imag());
#endif
}

//==============================================================================
// TimingRecovery Implementation
//==============================================================================

#ifdef ATSC3_FIXED_POINT
TimingRecovery::TimingRecovery(const TimingRecoveryConfig& config)
    : config_(config),
      interpolator_(config.polyphase),
      mu_q16_(0),
      timing_error_q15_(0),
      loop_integrator_q15_(0),
      kp_q15_(0),
      ki_q15_(0),
      sample_count_(0),
      symbol_count_(0),
      samples_since_symbol_(0),
      locked_(false) {
    size_t buf_size = config_.polyphase.taps_per_phase * 4;
    buffer_.resize(buf_size, sample_t(0, 0));
    buf_write_idx_ = 0;
    buf_read_idx_ = 0;
    buf_count_ = 0;

    set_timing_offset(config.initial_offset);
    compute_loop_gains();
}
#else
TimingRecovery::TimingRecovery(const TimingRecoveryConfig& config)
    : config_(config),
      interpolator_(config.polyphase),
      mu_(config.initial_offset),
      timing_error_(0.0),
      loop_integrator_(0.0),
      sample_count_(0),
      symbol_count_(0),
      samples_since_symbol_(0),
      locked_(false) {
    // Allocate sample buffer
    size_t buf_size = config_.polyphase.taps_per_phase * 4;  // Extra room
    buffer_.resize(buf_size, sample_t(0, 0));
    buf_write_idx_ = 0;
    buf_read_idx_ = 0;
    buf_count_ = 0;

    compute_loop_gains();
}
#endif

TimingRecovery::~TimingRecovery() = default;

void TimingRecovery::compute_loop_gains() {
    // Second-order loop design (one-time filter design in double -- same
    // reasoning as PolyphaseInterpolator::init_coefficients() above; the
    // per-sample use of kp_/ki_ in update_loop() is genuine fixed-point).
    // Natural frequency from loop bandwidth
    double wn = config_.loop_bandwidth_hz * kTwoPi;
    double zeta = config_.loop_damping;

    // Normalized to symbol rate
    double Ts = 1.0 / config_.symbol_rate_hz;
    double K = 1.0;  // TED gain estimate (signal-dependent)

    // Loop filter gains for type-2 loop
    // G(z) = Kp + Ki / (1 - z^-1)
    double kp = (4.0 * zeta * wn * Ts) / K;
    double ki = (4.0 * wn * wn * Ts * Ts) / K;

    // Limit gains for stability
    kp = std::min(kp, 0.1);
    ki = std::min(ki, 0.01);

#ifdef ATSC3_FIXED_POINT
    kp_q15_ = float_to_q15(static_cast<float>(kp));
    ki_q15_ = float_to_q15(static_cast<float>(ki));
#else
    kp_ = kp;
    ki_ = ki;
#endif
}

void TimingRecovery::reset() {
#ifdef ATSC3_FIXED_POINT
    set_timing_offset(config_.initial_offset);
    timing_error_q15_ = 0;
    loop_integrator_q15_ = 0;
#else
    mu_ = config_.initial_offset;
    timing_error_ = 0.0;
    loop_integrator_ = 0.0;
#endif
    sample_count_ = 0;
    symbol_count_ = 0;
    samples_since_symbol_ = 0;
    locked_ = false;

    std::fill(buffer_.begin(), buffer_.end(), sample_t(0, 0));
    buf_write_idx_ = 0;
    buf_read_idx_ = 0;
    buf_count_ = 0;

    ted_.reset();
}

void TimingRecovery::set_timing_offset(double mu) {
    double wrapped = mu - std::floor(mu);  // Keep in [0, 1)
#ifdef ATSC3_FIXED_POINT
    mu_q16_ = static_cast<uint16_t>(static_cast<int64_t>(wrapped * 65536.0));
#else
    mu_ = wrapped;
#endif
}

void TimingRecovery::update_loop(double error) {
    if (!locked_) {
        return;  // Don't update when unlocked
    }

#ifdef ATSC3_FIXED_POINT
    // error comes from GardnerTed::compute_error(), which is not bounded
    // to Q1.15's [-1, 1) (it's a diff*mid product, not normalized sample
    // data) -- converting it with the ordinary saturating float_to_q15()
    // would silently clip large error signals exactly like the bug found
    // and fixed in bootstrap_detector's metric computation. Use a plain
    // (non-saturating) wide conversion instead.
    int64_t error_q15 = static_cast<int64_t>(error * 32768.0);

    // Second-order loop filter, all in Q1.15-scaled fixed point.
    int64_t ki_term = (static_cast<int64_t>(ki_q15_) * error_q15) >> 15;
    loop_integrator_q15_ += ki_term;

    int64_t kp_term = (static_cast<int64_t>(kp_q15_) * error_q15) >> 15;
    int64_t adjustment_q15 = kp_term + loop_integrator_q15_;

    // mu_q16_ is Q0.16 (see the header comment); adjustment_q15 is
    // Q1.15-scaled, so converting to Q0.16 units is one left shift.
    // Adding to mu_q16_ via a signed intermediate and truncating back to
    // uint16_t wraps to [0, 1) for free (well-defined modulo-2^16
    // conversion), regardless of adjustment's sign or magnitude -- no
    // compare-and-subtract needed, matching the CORDIC angle format's
    // half-turn trick.
    int64_t adjustment_q16 = adjustment_q15 << 1;
    mu_q16_ = static_cast<uint16_t>(static_cast<int64_t>(mu_q16_) + adjustment_q16);

    // Filtered error: timing_error_ = 0.9*timing_error_ + 0.1*error,
    // exact Q1.15 gains (not a power-of-2 approximation -- unlike
    // bootstrap_detector's EWMA, there's no natural simplification here,
    // and a real fixed-point multiply is ordinary, realistic RTL work).
    constexpr int64_t kA = 29491;  // float_to_q15(0.9f)
    constexpr int64_t kB = 3277;   // float_to_q15(0.1f)
    timing_error_q15_ = (timing_error_q15_ * kA + error_q15 * kB) >> 15;
#else
    // Second-order loop filter
    loop_integrator_ += ki_ * error;

    // Total timing adjustment
    double adjustment = kp_ * error + loop_integrator_;

    // Update fractional offset
    mu_ += adjustment;

    // Wrap to [0, 1)
    while (mu_ >= 1.0) {
        mu_ -= 1.0;
    }
    while (mu_ < 0.0) {
        mu_ += 1.0;
    }

    // Store filtered error
    timing_error_ = 0.9 * timing_error_ + 0.1 * error;
#endif
}

void TimingRecovery::emit_symbol() {
    if (!symbol_callback_ || buf_count_ < config_.polyphase.taps_per_phase) {
        return;
    }

    // Interpolate at current timing offset. get_timing_offset() reads
    // whichever representation (mu_ or mu_q16_) this build uses and
    // returns it as the double interpolate() expects -- interpolate()
    // itself re-wraps to [0, 1) internally regardless of build mode, so
    // no separate fixed-point wraparound is needed just to read mu here
    // (see set_timing_offset()/update_loop() for where the Q0.16
    // representation's wraparound actually matters: across many
    // accumulated loop-filter updates).
    double mu = get_timing_offset();
    sample_t symbol = interpolator_.interpolate(buffer_.data(), buf_read_idx_, mu, buffer_.size());

    // Call user callback
    symbol_callback_(symbol, mu);

    ++symbol_count_;
}

void TimingRecovery::process_sample(const sample_t& sample) {
    // Add to buffer
    buffer_[buf_write_idx_] = sample;
    buf_write_idx_ = (buf_write_idx_ + 1) % buffer_.size();
    if (buf_count_ < buffer_.size()) {
        ++buf_count_;
    }

    ++sample_count_;
    ++samples_since_symbol_;

    // Check if we have a symbol boundary
    // For 2x oversampled signal, symbol every 2 samples
    size_t sps = config_.polyphase.samples_per_symbol;

    if (samples_since_symbol_ >= sps) {
        samples_since_symbol_ = 0;

        // Emit interpolated symbol
        emit_symbol();

        // Compute timing error if we have enough history
        if (buf_count_ >= config_.polyphase.taps_per_phase && locked_) {
            // Get samples for Gardner TED
            // Current symbol, midpoint, previous symbol
            size_t buf_sz = buffer_.size();
            size_t curr_idx = (buf_write_idx_ + buf_sz - 1) % buf_sz;
            size_t mid_idx = (buf_write_idx_ + buf_sz - 1 - sps / 2) % buf_sz;
            size_t prev_idx = (buf_write_idx_ + buf_sz - 1 - sps) % buf_sz;

            double mu = get_timing_offset();
            sample_t x_curr = interpolator_.interpolate(buffer_.data(), curr_idx, mu, buf_sz);
            sample_t x_mid = interpolator_.interpolate(buffer_.data(), mid_idx, mu + 0.5, buf_sz);
            sample_t x_prev = interpolator_.interpolate(buffer_.data(), prev_idx, mu, buf_sz);

            double error = ted_.compute_error(x_curr, x_mid, x_prev);
            update_loop(error);
        }

        // Advance read pointer
        buf_read_idx_ = (buf_read_idx_ + sps) % buffer_.size();
    }
}

void TimingRecovery::process(const sample_t* in, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        process_sample(in[i]);
    }
}

}  // namespace sync
}  // namespace atsc3
