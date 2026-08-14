// bootstrap_detector.cc — Bootstrap Detector implementation
//
// Schmidl-Cox autocorrelation for ATSC 3.0 bootstrap detection

#include "bootstrap_detector.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace sync {

BootstrapDetector::BootstrapDetector(const BootstrapConfig& config)
    : config_(config),
      p_sum_(0.0, 0.0),
      r_sum_(0.0),
      delay_buffer_(kHalfSymbol, sample_t(0, 0)),
      delay_idx_(0),
      power_buffer_(kHalfSymbol, 0.0),
      power_idx_(0),
      metric_history_(std::max<size_t>(1, config.averaging_window), 0.0),
      metric_idx_(0),
      metric_sum_(0.0),
      sample_count_(0),
      in_detection_(false),
      peak_metric_(0.0),
      peak_sample_(0),
      peak_correlation_(0.0, 0.0) {}

BootstrapDetector::~BootstrapDetector() = default;

void BootstrapDetector::reset() {
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
}

void BootstrapDetector::set_config(const BootstrapConfig& config) {
    config_ = config;

    // Resize metric history if needed. averaging_window is clamped to a
    // minimum of 1: a size-0 metric_history_ makes check_detection()'s
    // modulo-by-size and vector index into it undefined behavior.
    size_t window = std::max<size_t>(1, config.averaging_window);
    if (metric_history_.size() != window) {
        metric_history_.resize(window, 0.0);
        metric_idx_ = 0;
        metric_sum_ = 0.0;
    }
}

void BootstrapDetector::process_sample(sample_t sample) {
    // Convert sample to double precision for accumulation
#ifdef ATSC3_FIXED_POINT
    double re = q15_to_float(sample.real());
    double im = q15_to_float(sample.imag());
#else
    double re = static_cast<double>(sample.real());
    double im = static_cast<double>(sample.imag());
#endif
    std::complex<double> x(re, im);

    // Get delayed sample (L = kHalfSymbol samples ago)
    sample_t delayed_sample = delay_buffer_[delay_idx_];
#ifdef ATSC3_FIXED_POINT
    double del_re = q15_to_float(delayed_sample.real());
    double del_im = q15_to_float(delayed_sample.imag());
#else
    double del_re = static_cast<double>(delayed_sample.real());
    double del_im = static_cast<double>(delayed_sample.imag());
#endif
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
}

BootstrapDetection BootstrapDetector::check_detection() {
    BootstrapDetection result;

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

    return result;
}

BootstrapDetection BootstrapDetector::process(sample_t sample) {
    process_sample(sample);
    return check_detection();
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
