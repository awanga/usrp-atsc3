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
    const BootstrapConfig& get_config() const { return config_; }

    // Update configuration
    void set_config(const BootstrapConfig& config);

    // Get current sample count (since reset)
    size_t get_sample_count() const { return sample_count_; }

private:
    BootstrapConfig config_;

    // Schmidl-Cox state
    // P = sum of x[n] * conj(x[n - L]) for n in window
    // R = sum of |x[n - L]|^2 for n in window
    std::complex<double> p_sum_;
    double r_sum_;

    // Circular buffer for delayed samples (size = kHalfSymbol)
    std::vector<sample_t> delay_buffer_;
    size_t delay_idx_;

    // Circular buffer for power computation (size = kHalfSymbol)
    std::vector<double> power_buffer_;
    size_t power_idx_;

    // Metric history for smoothing
    std::vector<double> metric_history_;
    size_t metric_idx_;
    double metric_sum_;

    // Detection state
    size_t sample_count_;
    bool in_detection_;
    double peak_metric_;
    size_t peak_sample_;
    std::complex<double> peak_correlation_;

    // Process one sample (internal)
    void process_sample(sample_t sample);

    // Check for detection peak
    BootstrapDetection check_detection();
};

}  // namespace sync
}  // namespace atsc3
