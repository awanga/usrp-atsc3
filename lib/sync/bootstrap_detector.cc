// bootstrap_detector.cc — Bootstrap Detector implementation
//
// Schmidl-Cox autocorrelation for ATSC 3.0 bootstrap detection

#include "bootstrap_detector.h"

#include "dsp/cordic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atsc3 {
namespace sync {

namespace {

// EWMA decay for both P and R: x - x/1024 per sample (a power of two, so
// the fixed-point update is one shift). With P's per-sample input
// 2 * corr and R's (|x|^2 + |x_d|^2), the steady-state scale of both is
// that of a 2048-sample (kHalfSymbol) window, and |P| <= R holds exactly
// because |corr| <= (|x|^2 + |x_d|^2) / 2 term by term.
constexpr int kEwmaShift = 10;

// Near-silence floor on R, below which the metric is forced to 0. In raw
// Q1.15 x Q1.15 product units (the fixed-point R's scale): R ~= 1024 *
// (|x|^2 + |x_d|^2) at steady state, so 2^16 corresponds to an rms
// amplitude of about 4 LSB (~-78 dBFS) -- far below any real input,
// including a receiver's own noise floor after AGC. Without a floor,
// exact silence leaves both EWMAs decaying into their truncation
// residuals, whose ratio is arbitrary and can look like a correlation
// peak.
constexpr int64_t kMinCorrelatorEnergyQ30 = int64_t{1} << 16;

// SNR estimate from the peak metric, shared by both builds (unchanged
// formula: snr = M / (1 - sqrt(M))^2, clamped outside (0.1, 0.99)).
double snr_db_from_metric(double metric) {
    if (metric > 0.1 && metric < 0.99) {
        double sqrt_m = std::sqrt(metric);
        double snr_linear = metric / ((1.0 - sqrt_m) * (1.0 - sqrt_m));
        return 10.0 * std::log10(snr_linear);
    }
    if (metric >= 0.99) {
        return 30.0;
    }
    return 0.0;
}

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
#else
// The same floor in the float build's units (samples in [-1, 1), so
// Q1.15 x Q1.15 products are scaled by 2^-30).
constexpr double kMinCorrelatorEnergy = static_cast<double>(kMinCorrelatorEnergyQ30) / 1073741824.0;
constexpr double kEwmaDecay = 1.0 / static_cast<double>(1 << kEwmaShift);
#endif

}  // namespace

#ifdef ATSC3_FIXED_POINT
BootstrapDetector::BootstrapDetector(const BootstrapConfig& config)
    : config_(config),
      p_sum_re_(0),
      p_sum_im_(0),
      r_sum_(0),
      delay_buffer_(kHalfSymbol, sample_t(0, 0)),
      delay_idx_(0),
      metric_history_(std::max<size_t>(1, config.averaging_window), 0),
      metric_sum_(0),
      metric_idx_(0),
      sample_count_(0),
      in_detection_(false),
      rearm_blocked_(false),
      peak_metric_(0),
      peak_angle_q15_(0),
      peak_sample_(0),
      last_smoothed_metric_q15_(0),
      last_angle_q15_(0) {}
#else
BootstrapDetector::BootstrapDetector(const BootstrapConfig& config)
    : config_(config),
      p_sum_(0.0, 0.0),
      r_sum_(0.0),
      delay_buffer_(kHalfSymbol, sample_t(0, 0)),
      delay_idx_(0),
      metric_history_(std::max<size_t>(1, config.averaging_window), 0.0),
      metric_sum_(0.0),
      metric_idx_(0),
      sample_count_(0),
      in_detection_(false),
      rearm_blocked_(false),
      peak_metric_(0.0),
      peak_correlation_(0.0, 0.0),
      peak_sample_(0),
      last_smoothed_metric_(0.0),
      last_phase_(0.0) {}
#endif

BootstrapDetector::~BootstrapDetector() = default;

void BootstrapDetector::reset() {
#ifdef ATSC3_FIXED_POINT
    p_sum_re_ = 0;
    p_sum_im_ = 0;
    r_sum_ = 0;
    std::fill(metric_history_.begin(), metric_history_.end(), 0);
    metric_sum_ = 0;
    peak_metric_ = 0;
    peak_angle_q15_ = 0;
    last_smoothed_metric_q15_ = 0;
    last_angle_q15_ = 0;
#else
    p_sum_ = std::complex<double>(0.0, 0.0);
    r_sum_ = 0.0;
    std::fill(metric_history_.begin(), metric_history_.end(), 0.0);
    metric_sum_ = 0.0;
    peak_metric_ = 0.0;
    peak_correlation_ = std::complex<double>(0.0, 0.0);
    last_smoothed_metric_ = 0.0;
    last_phase_ = 0.0;
#endif
    std::fill(delay_buffer_.begin(), delay_buffer_.end(), sample_t(0, 0));
    delay_idx_ = 0;
    metric_idx_ = 0;
    sample_count_ = 0;
    in_detection_ = false;
    rearm_blocked_ = false;
    peak_sample_ = 0;
}

void BootstrapDetector::set_config(const BootstrapConfig& config) {
    config_ = config;

    // Resize metric history if needed. averaging_window is clamped to a
    // minimum of 1: a size-0 metric_history_ makes check_detection()'s
    // modulo-by-size and vector index into it undefined behavior.
    size_t window = std::max<size_t>(1, config.averaging_window);
    if (metric_history_.size() != window) {
#ifdef ATSC3_FIXED_POINT
        metric_history_.resize(window, 0);
        metric_idx_ = 0;
        metric_sum_ = 0;
#else
        metric_history_.resize(window, 0.0);
        metric_idx_ = 0;
        metric_sum_ = 0.0;
#endif
    }
}

void BootstrapDetector::process_sample(sample_t sample) {
    sample_t delayed_sample = delay_buffer_[delay_idx_];

#ifdef ATSC3_FIXED_POINT
    int64_t x_re = sample.real();
    int64_t x_im = sample.imag();
    int64_t xd_re = delayed_sample.real();
    int64_t xd_im = delayed_sample.imag();

    // x[n] * conj(x[n-L]) and |x[n]|^2 + |x[n-L]|^2, kept as raw
    // (unshifted) Q1.15 x Q1.15 products: P and R share this scale, so it
    // cancels exactly in check_detection()'s P/R ratio.
    int64_t corr_re = x_re * xd_re + x_im * xd_im;
    int64_t corr_im = x_im * xd_re - x_re * xd_im;
    int64_t energy = x_re * x_re + x_im * x_im + xd_re * xd_re + xd_im * xd_im;

    // Both EWMAs decay by x >> 10 (floor division, identically for both);
    // P's input is 2 * corr and R's is the two-half energy, so |P| <= R.
    // Bounds: |corr| <= 2^31 and energy <= 2^32 per sample, so both
    // accumulators stay below ~2^43.
    p_sum_re_ = p_sum_re_ - (p_sum_re_ >> kEwmaShift) + 2 * corr_re;
    p_sum_im_ = p_sum_im_ - (p_sum_im_ >> kEwmaShift) + 2 * corr_im;
    r_sum_ = r_sum_ - (r_sum_ >> kEwmaShift) + energy;
#else
    std::complex<double> x(sample.real(), sample.imag());
    std::complex<double> x_delayed(delayed_sample.real(), delayed_sample.imag());

    std::complex<double> corr = x * std::conj(x_delayed);
    double energy = std::norm(x) + std::norm(x_delayed);

    p_sum_ = p_sum_ - p_sum_ * kEwmaDecay + 2.0 * corr;
    r_sum_ = r_sum_ - r_sum_ * kEwmaDecay + energy;
#endif

    delay_buffer_[delay_idx_] = sample;
    delay_idx_ = (delay_idx_ + 1) % kHalfSymbol;

    ++sample_count_;
}

BootstrapDetection BootstrapDetector::check_detection() {
    BootstrapDetection result;

#ifdef ATSC3_FIXED_POINT
    // |P|/R in Q1.15. |P| <= R by construction (see process_sample()), so
    // the quotient fits Q1.15 up to the EWMAs' truncation rounding, which
    // the saturation absorbs. P * 32768 < 2^58, no overflow. Below the
    // energy floor the vector is (0, 0): CORDIC then returns magnitude 0
    // and angle 0, so the metric is 0 without a separate path.
    int16_t norm_re_q15 = 0;
    int16_t norm_im_q15 = 0;
    if (r_sum_ >= kMinCorrelatorEnergyQ30) {
        norm_re_q15 = saturate_i16((p_sum_re_ * 32768) / r_sum_);
        norm_im_q15 = saturate_i16((p_sum_im_ * 32768) / r_sum_);
    }

    dsp::CordicVectorResult vec = dsp::cordic_vector(norm_re_q15, norm_im_q15);

    // metric = (|P|/R)^2 in Q1.15. magnitude <= ~46341 (both rails at
    // +-32767), so metric <= 65536: no overflow anywhere.
    int64_t magnitude = vec.magnitude;
    int32_t metric = static_cast<int32_t>((magnitude * magnitude) >> 15);

    // Smooth metric with moving average
    metric_sum_ -= metric_history_[metric_idx_];
    metric_sum_ += metric;
    metric_history_[metric_idx_] = metric;
    metric_idx_ = (metric_idx_ + 1) % metric_history_.size();

    int64_t smoothed_metric = metric_sum_ / static_cast<int64_t>(metric_history_.size());
    int32_t threshold_q15 = float_to_q15(static_cast<float>(config_.threshold));

    // Stash for get_current_metric()/get_current_cfo_hz(), independent of
    // whether the detection FSM below fires this call.
    last_smoothed_metric_q15_ = smoothed_metric;
    last_angle_q15_ = vec.angle_q15;

    if (!in_detection_) {
        if (rearm_blocked_) {
            if (smoothed_metric <= threshold_q15) {
                rearm_blocked_ = false;
            }
        } else if (smoothed_metric > threshold_q15) {
            in_detection_ = true;
            peak_metric_ = static_cast<int32_t>(smoothed_metric);
            peak_sample_ = sample_count_;
            peak_angle_q15_ = vec.angle_q15;
        }
    } else {
        if (smoothed_metric > peak_metric_) {
            peak_metric_ = static_cast<int32_t>(smoothed_metric);
            peak_sample_ = sample_count_;
            peak_angle_q15_ = vec.angle_q15;
        }

        // Falling edge: smoothed_metric < peak_metric_ * 0.8
        int64_t falling_threshold = (static_cast<int64_t>(peak_metric_) * float_to_q15(0.8f)) >> 15;
        if (smoothed_metric < falling_threshold) {
            result.detected = true;
            result.sample_index = peak_sample_;
            result.metric = static_cast<double>(peak_metric_) / 32768.0;

            // CFO from the CORDIC phase at the peak. The pi in
            // "phase = 2*pi*CFO*L/Fs" cancels exactly because the CORDIC
            // angle format is already radians/pi-scaled (see
            // lib/dsp/cordic.h), so this is a plain integer multiply plus
            // a power-of-2 shift -- no floating-point trig, no division
            // by pi, anywhere in this path:
            //   cfo_hz = phase_rad * Fs / (2*pi*L)
            //          = (angle_q15/32768*pi) * Fs / (2*pi*L)
            //          = angle_q15 * Fs / (65536*L)
            //          = (angle_q15 * Fs_int) >> 27      [65536*2048 == 2^27]
            int64_t sample_rate_hz_int = static_cast<int64_t>(config_.sample_rate_hz);
            int64_t cfo_hz_int = (static_cast<int64_t>(peak_angle_q15_) * sample_rate_hz_int) >> 27;
            result.cfo_hz = static_cast<double>(cfo_hz_int);

            // snr_db is std::log10-based, which this milestone's CORDIC
            // core does not cover (rotation/vectoring modes only --
            // log10 needs hyperbolic mode, a separate design; see
            // hdl/docs/placeholder_status.md and
            // hdl/rtl/include/status_words.vh, which excludes this same
            // field from the RTL wire layout for the same reason).
            // Computed in double from the already-fixed-point
            // peak_metric_, at this one output boundary only.
            result.snr_db = snr_db_from_metric(static_cast<double>(peak_metric_) / 32768.0);

            in_detection_ = false;
            rearm_blocked_ = true;
            peak_metric_ = 0;
        }
    }
#else
    bool energetic = r_sum_ >= kMinCorrelatorEnergy;
    double metric = energetic ? std::norm(p_sum_) / (r_sum_ * r_sum_) : 0.0;
    std::complex<double> p_eff = energetic ? p_sum_ : std::complex<double>(0.0, 0.0);

    // Smooth metric with moving average
    metric_sum_ -= metric_history_[metric_idx_];
    metric_sum_ += metric;
    metric_history_[metric_idx_] = metric;
    metric_idx_ = (metric_idx_ + 1) % metric_history_.size();

    double smoothed_metric = metric_sum_ / static_cast<double>(metric_history_.size());

    // Stash for get_current_metric()/get_current_cfo_hz(), independent of
    // whether the detection FSM below fires this call.
    last_smoothed_metric_ = smoothed_metric;
    last_phase_ = std::arg(p_eff);

    if (!in_detection_) {
        if (rearm_blocked_) {
            if (smoothed_metric <= config_.threshold) {
                rearm_blocked_ = false;
            }
        } else if (smoothed_metric > config_.threshold) {
            in_detection_ = true;
            peak_metric_ = smoothed_metric;
            peak_sample_ = sample_count_;
            peak_correlation_ = p_eff;
        }
    } else {
        if (smoothed_metric > peak_metric_) {
            peak_metric_ = smoothed_metric;
            peak_sample_ = sample_count_;
            peak_correlation_ = p_eff;
        }

        // Falling edge detection (peak passed)
        if (smoothed_metric < peak_metric_ * 0.8) {
            result.detected = true;
            result.sample_index = peak_sample_;
            result.metric = peak_metric_;

            // Phase = 2 * pi * CFO * L / Fs  =>  CFO = phase * Fs / (2 * pi * L)
            double phase = std::arg(peak_correlation_);
            result.cfo_hz = phase * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);
            result.snr_db = snr_db_from_metric(peak_metric_);

            in_detection_ = false;
            rearm_blocked_ = true;
            peak_metric_ = 0.0;
        }
    }
#endif

    return result;
}

BootstrapDetection BootstrapDetector::process(sample_t sample) {
    process_sample(sample);
    return check_detection();
}

double BootstrapDetector::get_current_metric() const {
#ifdef ATSC3_FIXED_POINT
    return static_cast<double>(last_smoothed_metric_q15_) / 32768.0;
#else
    return last_smoothed_metric_;
#endif
}

double BootstrapDetector::get_current_cfo_hz() const {
#ifdef ATSC3_FIXED_POINT
    // Same pi-cancels-out derivation as the peak CFO in check_detection().
    int64_t sample_rate_hz_int = static_cast<int64_t>(config_.sample_rate_hz);
    int64_t cfo_hz_int = (static_cast<int64_t>(last_angle_q15_) * sample_rate_hz_int) >> 27;
    return static_cast<double>(cfo_hz_int);
#else
    return last_phase_ * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);
#endif
}

BootstrapDetection BootstrapDetector::process(const sample_t* samples, size_t n) {
    BootstrapDetection last_detection;

    for (size_t i = 0; i < n; ++i) {
        process_sample(samples[i]);
        BootstrapDetection det = check_detection();
        if (det.detected) {
            last_detection = det;
        }
    }

    return last_detection;
}

}  // namespace sync
}  // namespace atsc3
