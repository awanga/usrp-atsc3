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

}  // namespace

//==============================================================================
// PolyphaseInterpolator Implementation
//==============================================================================

PolyphaseInterpolator::PolyphaseInterpolator(const PolyphaseConfig& config)
    : config_(config) {
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

    // Design prototype filter (interpolation filter)
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
            coeffs_[p][t] = static_cast<float>(prototype[idx]);
        }
    }
}

sample_t PolyphaseInterpolator::interpolate(const sample_t* buf, size_t buf_idx,
                                             double mu) const {
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

#ifdef ATSC3_FIXED_POINT
    // Fixed-point accumulation in higher precision
    double acc_re = 0.0;
    double acc_im = 0.0;

    for (size_t t = 0; t < n_taps; ++t) {
        // Index into circular buffer (going backwards)
        size_t idx = (buf_idx + n_taps - 1 - t) % n_taps;
        acc_re += q15_to_float(buf[idx].real()) * taps[t];
        acc_im += q15_to_float(buf[idx].imag()) * taps[t];
    }

    return sample_t(float_to_q15(static_cast<float>(acc_re)),
                    float_to_q15(static_cast<float>(acc_im)));
#else
    float acc_re = 0.0f;
    float acc_im = 0.0f;

    for (size_t t = 0; t < n_taps; ++t) {
        size_t idx = (buf_idx + n_taps - 1 - t) % n_taps;
        acc_re += buf[idx].real() * taps[t];
        acc_im += buf[idx].imag() * taps[t];
    }

    return sample_t(acc_re, acc_im);
#endif
}

//==============================================================================
// GardnerTed Implementation
//==============================================================================

GardnerTed::GardnerTed()
    : prev_symbol_(0, 0), prev_mid_(0, 0), have_prev_(false) {}

GardnerTed::~GardnerTed() = default;

void GardnerTed::reset() {
    prev_symbol_ = sample_t(0, 0);
    prev_mid_ = sample_t(0, 0);
    have_prev_ = false;
}

double GardnerTed::compute_error(sample_t x_curr, sample_t x_mid, sample_t x_prev) const {
    // Gardner TED: e = Re{ (x_curr - x_prev) * conj(x_mid) }
#ifdef ATSC3_FIXED_POINT
    double curr_re = q15_to_float(x_curr.real());
    double curr_im = q15_to_float(x_curr.imag());
    double mid_re = q15_to_float(x_mid.real());
    double mid_im = q15_to_float(x_mid.imag());
    double prev_re = q15_to_float(x_prev.real());
    double prev_im = q15_to_float(x_prev.imag());

    double diff_re = curr_re - prev_re;
    double diff_im = curr_im - prev_im;

    // Real part of (diff * conj(mid))
    return diff_re * mid_re + diff_im * mid_im;
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

TimingRecovery::~TimingRecovery() = default;

void TimingRecovery::compute_loop_gains() {
    // Second-order loop design
    // Natural frequency from loop bandwidth
    double wn = config_.loop_bandwidth_hz * kTwoPi;
    double zeta = config_.loop_damping;

    // Normalized to symbol rate
    double Ts = 1.0 / config_.symbol_rate_hz;
    double K = 1.0;  // TED gain estimate (signal-dependent)

    // Loop filter gains for type-2 loop
    // G(z) = Kp + Ki / (1 - z^-1)
    kp_ = (4.0 * zeta * wn * Ts) / K;
    ki_ = (4.0 * wn * wn * Ts * Ts) / K;

    // Limit gains for stability
    kp_ = std::min(kp_, 0.1);
    ki_ = std::min(ki_, 0.01);
}

void TimingRecovery::reset() {
    mu_ = config_.initial_offset;
    timing_error_ = 0.0;
    loop_integrator_ = 0.0;
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
    mu_ = mu - std::floor(mu);  // Keep in [0, 1)
}

void TimingRecovery::update_loop(double error) {
    if (!locked_) {
        return;  // Don't update when unlocked
    }

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
}

void TimingRecovery::emit_symbol() {
    if (!symbol_callback_ || buf_count_ < config_.polyphase.taps_per_phase) {
        return;
    }

    // Interpolate at current timing offset
    sample_t symbol = interpolator_.interpolate(buffer_.data(), buf_read_idx_, mu_);

    // Call user callback
    symbol_callback_(symbol, mu_);

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

            sample_t x_curr = interpolator_.interpolate(buffer_.data(), curr_idx, mu_);
            sample_t x_mid = interpolator_.interpolate(buffer_.data(), mid_idx, mu_ + 0.5);
            sample_t x_prev = interpolator_.interpolate(buffer_.data(), prev_idx, mu_);

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
