#pragma once

// Bootstrap Detector — ATSC 3.0 frame detection and coarse CFO estimation
//
// Uses Schmidl-Cox autocorrelation to detect the bootstrap signal.
// Bootstrap is always 4096 samples regardless of data FFT size.
// Reference: ATSC A/322 Section 5.2
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (streaming IQ samples)
//   Output: Detection event with sample index and CFO estimate

#include "types.h"

#include <complex>
#include <cstddef>
#include <functional>

namespace atsc3 {
namespace sync {

// Detection result from bootstrap detector
struct BootstrapDetection {
    bool detected = false;

    // Sample index where bootstrap was detected
    // (relative to start of processing)
    size_t sample_index = 0;

    // Coarse CFO estimate in Hz
    // Derived from phase of autocorrelation
    double cfo_hz = 0.0;

    // Detection metric (0-1 normalized)
    // Higher = stronger detection
    double metric = 0.0;

    // Estimated SNR at detection point (dB)
    double snr_db = 0.0;
};

// Bootstrap Detector configuration
struct BootstrapConfig {
    // Sample rate in Hz (needed to convert CFO to Hz)
    double sample_rate_hz = 6.25e6;

    // Detection threshold (0-1)
    // Higher = fewer false detections but may miss weak signals
    double threshold = 0.7;

    // Averaging window for metric smoothing (samples)
    size_t averaging_window = 64;
};

// Bootstrap Detector
//
// Usage:
//   BootstrapConfig config;
//   config.sample_rate_hz = 6.25e6;
//   BootstrapDetector detector(config);
//
//   // Process streaming samples
//   while (has_samples) {
//       BootstrapDetection det = detector.process(sample);
//       if (det.detected) {
//           // Found bootstrap at det.sample_index
//           // Apply CFO correction of det.cfo_hz
//       }
//   }
//
class BootstrapDetector {
public:
    // Bootstrap symbol length (fixed by ATSC 3.0 spec)
    static constexpr size_t kBootstrapLength = 4096;

    // Half-symbol length for Schmidl-Cox
    static constexpr size_t kHalfSymbol = kBootstrapLength / 2;

    explicit BootstrapDetector(const BootstrapConfig& config = BootstrapConfig());
    ~BootstrapDetector();

    // Process one sample
    // Returns detection result (detected=false if no detection this sample)
    BootstrapDetection process(sample_t sample);

    // Process buffer of samples
    // Returns last detection result
    BootstrapDetection process(const sample_t* samples, size_t n);

    // Reset detector state
    void reset();

    // Get current configuration
    const BootstrapConfig& get_config() const {
        return config_;
    }

    // Update configuration
    void set_config(const BootstrapConfig& config);

    // Get current sample count (since reset)
    size_t get_sample_count() const {
        return sample_count_;
    }

    // Current (most recent) smoothed detection metric and CFO estimate,
    // updated every process()/process_sample() call regardless of
    // detection FSM state (unlike BootstrapDetection::metric/cfo_hz,
    // which are only meaningful when detected == true). Useful for
    // link-quality monitoring, and for the Phase 9.0b fixed-point-vs-
    // reference equivalence test (test_bootstrap_detector.cc), which
    // needs same-instant comparisons rather than only the FSM-gated
    // "detected" event -- two independently-evolving EWMAs (different
    // alpha) can trigger their peak/falling-edge detection on slightly
    // different samples even when tracking the same underlying signal.
    double get_current_metric() const;
    double get_current_cfo_hz() const;

private:
    BootstrapConfig config_;

    // Schmidl-Cox state
    // P = sum of x[n] * conj(x[n - L]) for n in window
    // R = sum of |x[n - L]|^2 for n in window
#ifdef ATSC3_FIXED_POINT
    // Genuine fixed-point state (Phase 9.0b rewrite), not double with
    // quantized I/O. p_sum_re_/p_sum_im_ hold the EWMA of
    // x[n]*conj(x[n-L]) as raw (unshifted) Q1.15 x Q1.15 products; the
    // shift-based alpha approximation (1/1024) and the rest of the fixed-
    // point design are documented in bootstrap_detector.cc.
    int64_t p_sum_re_;
    int64_t p_sum_im_;
    int64_t r_sum_;
#else
    std::complex<double> p_sum_;
    double r_sum_;
#endif

    // Circular buffer for delayed samples (size = kHalfSymbol)
    std::vector<sample_t> delay_buffer_;
    size_t delay_idx_;

    // Circular buffer for power computation (size = kHalfSymbol)
#ifdef ATSC3_FIXED_POINT
    std::vector<int64_t> power_buffer_;
#else
    std::vector<double> power_buffer_;
#endif
    size_t power_idx_;

    // Metric history for smoothing
#ifdef ATSC3_FIXED_POINT
    // Q1.15-scaled squared correlation ratio (see check_detection()).
    std::vector<int32_t> metric_history_;
    int64_t metric_sum_;
#else
    std::vector<double> metric_history_;
    double metric_sum_;
#endif
    size_t metric_idx_;

    // Detection state
    size_t sample_count_;
    bool in_detection_;
#ifdef ATSC3_FIXED_POINT
    int32_t peak_metric_;
    int16_t
        peak_angle_q15_;  // CORDIC angle format (dsp::cordic_vector); replaces peak_correlation_
#else
    double peak_metric_;
    std::complex<double> peak_correlation_;
#endif
    size_t peak_sample_;

    // Backing state for get_current_metric()/get_current_cfo_hz(),
    // stashed at the top of check_detection() every call.
#ifdef ATSC3_FIXED_POINT
    int64_t last_smoothed_metric_q15_;
    int16_t last_angle_q15_;
#else
    double last_smoothed_metric_;
    double last_phase_;
#endif

    // Process one sample (internal)
    void process_sample(sample_t sample);

    // Check for detection peak
    BootstrapDetection check_detection();
};

}  // namespace sync
}  // namespace atsc3
