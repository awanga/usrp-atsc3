// freq_correction.cc — Frequency Correction implementation
//
// CFO compensation using coarse (bootstrap) and fine (pilot) estimates

#include "freq_correction.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace atsc3 {
namespace sync {

//==============================================================================
// FreqCorrection Implementation
//==============================================================================

#ifdef ATSC3_FIXED_POINT
FreqCorrection::FreqCorrection(const FreqCorrectionConfig& config)
    : config_(config),
      coarse_cfo_hz_(config.initial_cfo_hz),
      fine_cfo_hz_(0.0),
      phase_increment_q31_(0),
      phase_accum_q31_(0),
      sample_count_(0) {
    update_phase_increment();
}
#else
FreqCorrection::FreqCorrection(const FreqCorrectionConfig& config)
    : config_(config),
      coarse_cfo_hz_(config.initial_cfo_hz),
      fine_cfo_hz_(0.0),
      phase_increment_(0.0),
      phase_(0.0),
      sample_count_(0) {
    update_phase_increment();
}
#endif

FreqCorrection::~FreqCorrection() = default;

void FreqCorrection::update_phase_increment() {
    double total_cfo = coarse_cfo_hz_ + fine_cfo_hz_;
    // Phase increment per sample = 2 * pi * CFO / Fs. Design-time (called
    // from set_coarse_cfo()/update_fine_cfo(), not per-sample) -- double
    // here is fine regardless of build mode, matching
    // TimingRecovery::compute_loop_gains(); only the per-sample datapath
    // in process() below needs to be genuinely fixed-point.
    double phase_increment_rad = -2.0 * M_PI * total_cfo / config_.sample_rate_hz;
#ifdef ATSC3_FIXED_POINT
    // Q1.31 "turns-over-pi": full int32_t range covers [-pi, pi), same
    // convention as cordic.h's Q1.15 angle format but with 16 more
    // fractional bits (see the header comment on phase_increment_q31_).
    double turns = phase_increment_rad / M_PI;
    double scaled = turns * 2147483648.0;
    if (scaled > 2147483647.0) {
        scaled = 2147483647.0;
    }
    if (scaled < -2147483648.0) {
        scaled = -2147483648.0;
    }
    phase_increment_q31_ = static_cast<int32_t>(scaled);
#else
    phase_increment_ = phase_increment_rad;
#endif
}

void FreqCorrection::set_coarse_cfo(double cfo_hz) {
    coarse_cfo_hz_ = cfo_hz;
    update_phase_increment();
}

void FreqCorrection::update_fine_cfo(double phase_error_per_sample) {
    if (!config_.enable_fine_tracking) {
        return;
    }

    // Convert phase error per sample to frequency (Hz)
    // phase_error = 2 * pi * f / Fs
    // f = phase_error * Fs / (2 * pi)
    double measured_cfo = phase_error_per_sample * config_.sample_rate_hz / (2.0 * M_PI);

    // Apply loop filter (simple first-order IIR)
    double alpha = config_.fine_loop_bandwidth_hz / config_.sample_rate_hz;
    alpha = std::min(alpha, 0.1);  // Limit bandwidth

    fine_cfo_hz_ = (1.0 - alpha) * fine_cfo_hz_ + alpha * measured_cfo;
    update_phase_increment();
}

void FreqCorrection::reset_phase() {
#ifdef ATSC3_FIXED_POINT
    phase_accum_q31_ = 0;
#else
    phase_ = 0.0;
#endif
}

double FreqCorrection::get_total_cfo() const {
    return coarse_cfo_hz_ + fine_cfo_hz_;
}

namespace {
#ifdef ATSC3_FIXED_POINT
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

void FreqCorrection::process(const sample_t* in, sample_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
#ifdef ATSC3_FIXED_POINT
        // Genuine fixed-point NCO (Phase 9.0b rewrite): CORDIC rotation
        // mode on native Q1.15 samples, no std::cos/sin and no
        // q15_to_float/float_to_q15 round trip at all. CORDIC's output is
        // bounded by construction (see lib/dsp/cordic.h), removing the
        // root cause of the unsaturated-cast bug this path used to have
        // -- not just clamping the symptom after the fact. Only the top
        // 16 bits of the wide phase accumulator address the rotation
        // (see the header comment on phase_accum_q31_).
        int16_t cordic_angle = static_cast<int16_t>(phase_accum_q31_ >> 16);
        dsp::CordicRotationResult rot = dsp::cordic_rotate(cordic_angle);
        int64_t in_re = in[i].real();
        int64_t in_im = in[i].imag();
        int64_t out_re = in_re * rot.cos_theta - in_im * rot.sin_theta;
        int64_t out_im = in_re * rot.sin_theta + in_im * rot.cos_theta;
        out[i] = sample_t(saturate_i16(out_re >> 15), saturate_i16(out_im >> 15));

        // Update phase accumulator. Wraps to [-pi, pi) for free via
        // integer overflow -- unsigned intermediate so the wraparound is
        // well-defined (int32_t + int32_t can genuinely overflow here,
        // unlike the 16-bit case elsewhere where both operands promote to
        // a wider int first).
        phase_accum_q31_ = static_cast<int32_t>(static_cast<uint32_t>(phase_accum_q31_) +
                                                static_cast<uint32_t>(phase_increment_q31_));
#else
        // Float mode: direct rotation
        double cos_p = std::cos(phase_);
        double sin_p = std::sin(phase_);
        float in_re = in[i].real();
        float in_im = in[i].imag();

        float out_re = in_re * static_cast<float>(cos_p) - in_im * static_cast<float>(sin_p);
        float out_im = in_re * static_cast<float>(sin_p) + in_im * static_cast<float>(cos_p);

        out[i] = sample_t(out_re, out_im);

        // Update phase accumulator
        phase_ += phase_increment_;

        // Wrap phase to [-pi, pi] to prevent numerical issues
        if (phase_ > M_PI) {
            phase_ -= 2.0 * M_PI;
        } else if (phase_ < -M_PI) {
            phase_ += 2.0 * M_PI;
        }
#endif
    }

    sample_count_ += n;
}

void FreqCorrection::process(sample_t* samples, size_t n) {
    // In-place processing - use same buffer for input and output
    for (size_t i = 0; i < n; ++i) {
#ifdef ATSC3_FIXED_POINT
        int16_t cordic_angle = static_cast<int16_t>(phase_accum_q31_ >> 16);
        dsp::CordicRotationResult rot = dsp::cordic_rotate(cordic_angle);
        int64_t in_re = samples[i].real();
        int64_t in_im = samples[i].imag();
        int64_t out_re = in_re * rot.cos_theta - in_im * rot.sin_theta;
        int64_t out_im = in_re * rot.sin_theta + in_im * rot.cos_theta;
        samples[i] = sample_t(saturate_i16(out_re >> 15), saturate_i16(out_im >> 15));

        phase_accum_q31_ = static_cast<int32_t>(static_cast<uint32_t>(phase_accum_q31_) +
                                                static_cast<uint32_t>(phase_increment_q31_));
#else
        double cos_p = std::cos(phase_);
        double sin_p = std::sin(phase_);
        float in_re = samples[i].real();
        float in_im = samples[i].imag();

        float out_re = in_re * static_cast<float>(cos_p) - in_im * static_cast<float>(sin_p);
        float out_im = in_re * static_cast<float>(sin_p) + in_im * static_cast<float>(cos_p);

        samples[i] = sample_t(out_re, out_im);

        phase_ += phase_increment_;
        if (phase_ > M_PI) {
            phase_ -= 2.0 * M_PI;
        } else if (phase_ < -M_PI) {
            phase_ += 2.0 * M_PI;
        }
#endif
    }

    sample_count_ += n;
}

//==============================================================================
// PilotPhaseTracker Implementation
//==============================================================================

PilotPhaseTracker::PilotPhaseTracker(const PilotPhaseTrackerConfig& config)
    : config_(config),
      phase_history_(config.averaging_window, 0.0),
      history_idx_(0),
      last_phase_(0.0),
      have_last_phase_(false),
      phase_error_per_subcarrier_(0.0),
      symbol_count_(0) {}

PilotPhaseTracker::~PilotPhaseTracker() = default;

void PilotPhaseTracker::reset() {
    std::fill(phase_history_.begin(), phase_history_.end(), 0.0);
    history_idx_ = 0;
    last_phase_ = 0.0;
    have_last_phase_ = false;
    phase_error_per_subcarrier_ = 0.0;
    symbol_count_ = 0;
}

double PilotPhaseTracker::unwrap_phase(double phase) {
    if (!have_last_phase_) {
        have_last_phase_ = true;
        last_phase_ = phase;
        return phase;
    }

    double delta = phase - last_phase_;

    // Unwrap
    while (delta > config_.unwrap_threshold) {
        phase -= 2.0 * M_PI;
        delta = phase - last_phase_;
    }
    while (delta < -config_.unwrap_threshold) {
        phase += 2.0 * M_PI;
        delta = phase - last_phase_;
    }

    last_phase_ = phase;
    return phase;
}

void PilotPhaseTracker::update(const double* phases, const size_t* indices, size_t n) {
    if (n < 2) {
        // Need at least 2 pilots to estimate slope
        return;
    }

    // Compute phase slope across subcarriers using least-squares
    // phase[k] = a + b * subcarrier_index[k]
    // We want b (slope per subcarrier)

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(indices[i]);
        double y = phases[i];  // Already in radians

        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }

    double n_d = static_cast<double>(n);
    double denom = n_d * sum_xx - sum_x * sum_x;

    if (std::abs(denom) < 1e-10) {
        return;  // Degenerate case
    }

    // Slope = phase error per subcarrier
    double slope = (n_d * sum_xy - sum_x * sum_y) / denom;

    // Unwrap the slope estimate
    slope = unwrap_phase(slope);

    // Update moving average
    phase_history_[history_idx_] = slope;
    history_idx_ = (history_idx_ + 1) % config_.averaging_window;

    // Compute average
    double sum = std::accumulate(phase_history_.begin(), phase_history_.end(), 0.0);
    phase_error_per_subcarrier_ = sum / static_cast<double>(config_.averaging_window);

    ++symbol_count_;
}

double PilotPhaseTracker::get_phase_error_per_sample() const {
    // Convert phase error per subcarrier to phase error per sample
    // Phase per subcarrier = phase per symbol / N_fft
    // For time-domain samples: phase_per_sample = phase_per_subcarrier * N_fft / N_symbol
    // But for CFO tracking we want frequency, not time offset

    // The phase slope across subcarriers is related to timing error, not CFO
    // For CFO, we want average phase change per symbol
    // This implementation returns the phase slope for now - actual CFO would
    // come from symbol-to-symbol phase change of pilots

    return phase_error_per_subcarrier_;
}

double PilotPhaseTracker::get_residual_cfo(double sample_rate_hz) const {
    // Convert phase error per sample to frequency
    // f = phase_per_sample * Fs / (2 * pi)
    double phase_per_sample = get_phase_error_per_sample();
    return phase_per_sample * sample_rate_hz / (2.0 * M_PI);
}

}  // namespace sync
}  // namespace atsc3
