// bootstrap_detector.cc — Bootstrap Detector implementation
//
// Schmidl-Cox autocorrelation for ATSC 3.0 bootstrap detection

#include "bootstrap_detector.h"

#include "dsp/cordic.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace sync {

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

#ifdef ATSC3_FIXED_POINT
BootstrapDetector::BootstrapDetector(const BootstrapConfig& config)
    : config_(config),
      p_sum_re_(0),
      p_sum_im_(0),
      r_sum_(0),
      delay_buffer_(kHalfSymbol, sample_t(0, 0)),
      delay_idx_(0),
      power_buffer_(kHalfSymbol, 0),
      power_idx_(0),
      metric_history_(std::max<size_t>(1, config.averaging_window), 0),
      metric_sum_(0),
      metric_idx_(0),
      sample_count_(0),
      in_detection_(false),
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
      power_buffer_(kHalfSymbol, 0.0),
      power_idx_(0),
      metric_history_(std::max<size_t>(1, config.averaging_window), 0.0),
      metric_sum_(0.0),
      metric_idx_(0),
      sample_count_(0),
      in_detection_(false),
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
    std::fill(delay_buffer_.begin(), delay_buffer_.end(), sample_t(0, 0));
    delay_idx_ = 0;
    std::fill(power_buffer_.begin(), power_buffer_.end(), 0);
    power_idx_ = 0;
    std::fill(metric_history_.begin(), metric_history_.end(), 0);
    metric_idx_ = 0;
    metric_sum_ = 0;
    sample_count_ = 0;
    in_detection_ = false;
    peak_metric_ = 0;
    peak_angle_q15_ = 0;
    peak_sample_ = 0;
    last_smoothed_metric_q15_ = 0;
    last_angle_q15_ = 0;
#else
    p_sum_ = std::complex<double>(0.0, 0.0);
    r_sum_ = 0.0;
    std::fill(delay_buffer_.begin(), delay_buffer_.end(), sample_t(0, 0));
    delay_idx_ = 0;
    std::fill(power_buffer_.begin(), power_buffer_.end(), 0.0);
    power_idx_ = 0;
    std::fill(metric_history_.begin(), metric_history_.end(), 0.0);
    metric_idx_ = 0;
    metric_sum_ = 0.0;
    sample_count_ = 0;
    in_detection_ = false;
    peak_metric_ = 0.0;
    peak_sample_ = 0;
    peak_correlation_ = std::complex<double>(0.0, 0.0);
    last_smoothed_metric_ = 0.0;
    last_phase_ = 0.0;
#endif
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
#ifdef ATSC3_FIXED_POINT
    // Genuine integer arithmetic throughout (Phase 9.0b rewrite) -- no
    // double, no per-sample float conversion. p_sum_/r_sum_ previously
    // only quantized I/O and computed internally in double; see the HDL
    // port plan's Decision 7.
    sample_t delayed_sample = delay_buffer_[delay_idx_];

    int64_t x_re = sample.real();
    int64_t x_im = sample.imag();
    int64_t xd_re = delayed_sample.real();
    int64_t xd_im = delayed_sample.imag();

    // x[n] * conj(x[n-L]), kept as raw (unshifted) Q1.15 x Q1.15 products.
    // Both p_sum_ and r_sum_ stay in this same unshifted scale throughout,
    // so it cancels exactly in check_detection()'s P/R ratio -- no
    // precision is lost by skipping the usual >>15 rescale here, and it's
    // one fewer shift per sample.
    int64_t corr_re = x_re * xd_re + x_im * xd_im;
    int64_t corr_im = x_im * xd_re - x_re * xd_im;

    int64_t old_power = power_buffer_[power_idx_];
    int64_t new_power = xd_re * xd_re + xd_im * xd_im;

    // Sliding window update for R (power sum)
    r_sum_ -= old_power;
    r_sum_ += new_power;
    if (r_sum_ < 1) {
        r_sum_ = 1;  // prevent division by zero (integer analog of the float path's 1e-20 floor)
    }

    power_buffer_[power_idx_] = new_power;
    power_idx_ = (power_idx_ + 1) % kHalfSymbol;

    // EWMA for P, alpha = 1/1024: a power-of-2 approximation of the float
    // path's 2/(kHalfSymbol+1) = 2/2049 (both are already approximations
    // of a true 2048-tap sliding sum -- the float path's own comment below
    // documents this as intentional, not a shortcut taken here). 1/1024
    // makes alpha*kHalfSymbol land on exactly 2, so the whole update is
    // one shift and one add, no fractional multiply:
    //   p_new = p_old*(1 - 1/1024) + (1/1024)*corr*2048
    //         = p_old - p_old/1024 + 2*corr
    p_sum_re_ = p_sum_re_ - (p_sum_re_ >> 10) + 2 * corr_re;
    p_sum_im_ = p_sum_im_ - (p_sum_im_ >> 10) + 2 * corr_im;

    delay_buffer_[delay_idx_] = sample;
    delay_idx_ = (delay_idx_ + 1) % kHalfSymbol;

    ++sample_count_;
#else
    // Convert sample to double precision for accumulation
    double re = static_cast<double>(sample.real());
    double im = static_cast<double>(sample.imag());
    std::complex<double> x(re, im);

    // Get delayed sample (L = kHalfSymbol samples ago)
    sample_t delayed_sample = delay_buffer_[delay_idx_];
    double del_re = static_cast<double>(delayed_sample.real());
    double del_im = static_cast<double>(delayed_sample.imag());
    std::complex<double> x_delayed(del_re, del_im);

    // Compute current correlation term: x[n] * conj(x[n-L])
    std::complex<double> corr_term = x * std::conj(x_delayed);

    // Get oldest correlation term to subtract (sliding window)
    // We need to track what we're removing from the sum
    // This requires storing the product, but we approximate by using
    // the oldest delayed sample's power contribution

    // Update power of delayed sample
    double old_power = power_buffer_[power_idx_];
    double new_power = std::norm(x_delayed);

    // Sliding window update for R (power sum)
    r_sum_ -= old_power;
    r_sum_ += new_power;
    r_sum_ = std::max(r_sum_, 1e-20);  // Prevent division by zero

    power_buffer_[power_idx_] = new_power;
    power_idx_ = (power_idx_ + 1) % kHalfSymbol;

    // For P (correlation sum), we use exponential averaging as approximation
    // True sliding window would require storing kHalfSymbol complex products
    constexpr double kAlpha = 2.0 / (kHalfSymbol + 1);
    p_sum_ = (1.0 - kAlpha) * p_sum_ + kAlpha * corr_term * static_cast<double>(kHalfSymbol);

    // Store current sample in delay buffer
    delay_buffer_[delay_idx_] = sample;
    delay_idx_ = (delay_idx_ + 1) % kHalfSymbol;

    ++sample_count_;
#endif
}

BootstrapDetection BootstrapDetector::check_detection() {
    BootstrapDetection result;

#ifdef ATSC3_FIXED_POINT
    // Normalize P by R first (both are in the same unshifted product
    // scale set up in process_sample(), so this ratio is exact regardless
    // of the accumulators' absolute magnitude). The result has the same
    // phase as p_sum_, since dividing by a positive real scalar (R)
    // doesn't change phase -- but it is *not* generally bounded to
    // Q1.15's [-1, 1): R is a true sliding-window sum that starts at 0
    // and only reaches its full kHalfSymbol-sample window gradually,
    // while P is an EWMA that responds faster, so right after a
    // correlation lag first engages, |P|/R legitimately overshoots 1.0
    // for a while (confirmed against the float path's own math -- this
    // is a real property of the class's documented EWMA-vs-sliding-window
    // approximation, not an error to clip away). Saturating here instead
    // of normalizing would silently clip that overshoot and bias the
    // metric during every such transient.
    //
    // CORDIC's angle output is unaffected by uniformly scaling both
    // components, so: right-shift both components by just enough to fit
    // int16_t (a block-floating-point-style normalization), run CORDIC,
    // then left-shift the magnitude output back by the same amount.
    int64_t norm_re = (p_sum_re_ * 32768) / r_sum_;
    int64_t norm_im = (p_sum_im_ * 32768) / r_sum_;

    int shift = 0;
    while ((norm_re > 32767 || norm_re < -32767 || norm_im > 32767 || norm_im < -32767) &&
           shift < 32) {
        norm_re >>= 1;
        norm_im >>= 1;
        ++shift;
    }
    int16_t norm_re_q15 = saturate_i16(norm_re);
    int16_t norm_im_q15 = saturate_i16(norm_im);

    dsp::CordicVectorResult vec = dsp::cordic_vector(norm_re_q15, norm_im_q15);
    int64_t magnitude = static_cast<int64_t>(vec.magnitude) << shift;

    // metric = (|P|/R)^2, in Q1.15 (magnitude is Q1.15-scaled |P|/R, now
    // correctly unbounded above rather than clipped to [0, 1]).
    int32_t metric = static_cast<int32_t>((magnitude * magnitude) >> 15);

    // Smooth metric with moving average (structurally unchanged from the
    // float path, now over integers).
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

    // Detection state machine (structurally unchanged from the float path)
    if (!in_detection_) {
        if (smoothed_metric > threshold_q15) {
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
            double peak_metric_f = static_cast<double>(peak_metric_) / 32768.0;
            if (peak_metric_f > 0.1 && peak_metric_f < 0.99) {
                double sqrt_m = std::sqrt(peak_metric_f);
                double snr_linear = peak_metric_f / ((1.0 - sqrt_m) * (1.0 - sqrt_m));
                result.snr_db = 10.0 * std::log10(snr_linear);
            } else if (peak_metric_f >= 0.99) {
                result.snr_db = 30.0;
            } else {
                result.snr_db = 0.0;
            }

            in_detection_ = false;
            peak_metric_ = 0;
        }
    }
#else
    // Compute Schmidl-Cox metric: M = |P|^2 / R^2
    double p_mag_sq = std::norm(p_sum_);
    double r_sq = r_sum_ * r_sum_;

    double metric = 0.0;
    if (r_sq > 1e-20) {
        metric = p_mag_sq / r_sq;
    }

    // Smooth metric with moving average
    metric_sum_ -= metric_history_[metric_idx_];
    metric_sum_ += metric;
    metric_history_[metric_idx_] = metric;
    metric_idx_ = (metric_idx_ + 1) % metric_history_.size();

    double smoothed_metric = metric_sum_ / static_cast<double>(metric_history_.size());

    // Stash for get_current_metric()/get_current_cfo_hz(), independent of
    // whether the detection FSM below fires this call.
    last_smoothed_metric_ = smoothed_metric;
    last_phase_ = std::arg(p_sum_);

    // Detection state machine
    if (!in_detection_) {
        // Look for rising edge above threshold
        if (smoothed_metric > config_.threshold) {
            in_detection_ = true;
            peak_metric_ = smoothed_metric;
            peak_sample_ = sample_count_;
            peak_correlation_ = p_sum_;
        }
    } else {
        // Track peak
        if (smoothed_metric > peak_metric_) {
            peak_metric_ = smoothed_metric;
            peak_sample_ = sample_count_;
            peak_correlation_ = p_sum_;
        }

        // Falling edge detection (peak passed)
        if (smoothed_metric < peak_metric_ * 0.8) {
            // Detection complete
            result.detected = true;
            result.sample_index = peak_sample_;
            result.metric = peak_metric_;

            // Compute CFO from correlation phase
            // Phase = 2 * pi * CFO * L / Fs
            // CFO = phase * Fs / (2 * pi * L)
            double phase = std::arg(peak_correlation_);
            result.cfo_hz = phase * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);

            // Estimate SNR from metric
            // metric = 1 / (1 + 1/SNR)^2 approximately
            // SNR ~= metric / (1 - sqrt(metric))^2 for high SNR
            if (peak_metric_ > 0.1 && peak_metric_ < 0.99) {
                double sqrt_m = std::sqrt(peak_metric_);
                double snr_linear = peak_metric_ / ((1.0 - sqrt_m) * (1.0 - sqrt_m));
                result.snr_db = 10.0 * std::log10(snr_linear);
            } else if (peak_metric_ >= 0.99) {
                result.snr_db = 30.0;  // Very high SNR
            } else {
                result.snr_db = 0.0;  // Low SNR
            }

            // Reset for next detection
            in_detection_ = false;
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
