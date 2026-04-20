#pragma once

// Frequency Correction — CFO compensation for OFDM signals
//
// Applies frequency offset correction to IQ samples.
// - Coarse CFO from bootstrap Schmidl-Cox estimate
// - Fine CFO from continual pilot phase tracking (residual)
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (uncorrected samples)
//   Output: TDATA=cf32, TVALID, TREADY (frequency-corrected samples)
//
// Reference: ATSC A/322 Section 5.2 (Bootstrap), Section 7.2 (Pilots)

#include "types.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace atsc3 {
namespace sync {

// Frequency Correction configuration
struct FreqCorrectionConfig {
    // Sample rate in Hz
    double sample_rate_hz = 6.25e6;

    // Initial CFO estimate (Hz) - typically from bootstrap detection
    double initial_cfo_hz = 0.0;

    // Enable fine CFO tracking from pilots
    bool enable_fine_tracking = true;

    // Fine CFO tracking loop bandwidth (Hz)
    // Lower = more stable, higher = faster adaptation
    double fine_loop_bandwidth_hz = 10.0;
};

// Frequency Correction processor
//
// Usage:
//   FreqCorrectionConfig config;
//   config.sample_rate_hz = 6.25e6;
//   config.initial_cfo_hz = bootstrap_detection.cfo_hz;
//   FreqCorrection corrector(config);
//
//   // Process samples
//   corrector.process(input, output, n_samples);
//
//   // Update fine CFO from pilot-based estimate
//   corrector.update_fine_cfo(pilot_phase_slope);
//
class FreqCorrection {
public:
    explicit FreqCorrection(const FreqCorrectionConfig& config = FreqCorrectionConfig());
    ~FreqCorrection();

    // Process samples with frequency correction
    // Applies current CFO estimate as complex rotation
    void process(const sample_t* in, sample_t* out, size_t n);

    // Process in-place
    void process(sample_t* samples, size_t n);

    // Set coarse CFO (Hz) - typically from bootstrap detection
    void set_coarse_cfo(double cfo_hz);

    // Update fine CFO from pilot-based measurement
    // phase_error: phase error in radians per sample
    void update_fine_cfo(double phase_error_per_sample);

    // Reset phase accumulator (e.g., on frame boundary)
    void reset_phase();

    // Get current total CFO estimate (coarse + fine)
    double get_total_cfo() const;

    // Get current phase accumulator value
    double get_phase() const {
        return phase_;
    }

    // Get configuration
    const FreqCorrectionConfig& get_config() const {
        return config_;
    }

    // Get sample count processed
    size_t get_sample_count() const {
        return sample_count_;
    }

private:
    FreqCorrectionConfig config_;

    // Coarse CFO from bootstrap (Hz)
    double coarse_cfo_hz_;

    // Fine CFO from pilot tracking (Hz)
    double fine_cfo_hz_;

    // Phase increment per sample (radians)
    double phase_increment_;

    // Current phase accumulator
    double phase_;

    // Sample counter
    size_t sample_count_;

    // Update phase increment from CFO
    void update_phase_increment();
};

// Continual Pilot Phase Tracker
//
// Estimates residual CFO from continual pilot phase drift.
// Used to feed fine CFO updates to FreqCorrection.
//
// Usage:
//   PilotPhaseTracker tracker(config);
//   // After each OFDM symbol
//   tracker.update(cp_phases, cp_indices, n_pilots);
//   double residual = tracker.get_phase_error_per_sample();
//   freq_corrector.update_fine_cfo(residual);
//
struct PilotPhaseTrackerConfig {
    // FFT size (needed to convert phase per subcarrier to phase per sample)
    size_t fft_size = 8192;

    // Averaging window for phase estimates (symbols)
    size_t averaging_window = 4;

    // Phase unwrap threshold (radians)
    double unwrap_threshold = M_PI;
};

class PilotPhaseTracker {
public:
    explicit PilotPhaseTracker(const PilotPhaseTrackerConfig& config = PilotPhaseTrackerConfig());
    ~PilotPhaseTracker();

    // Update with continual pilot phases for current symbol
    // phases: observed phase of each continual pilot (radians)
    // indices: subcarrier index of each continual pilot
    // n: number of continual pilots
    void update(const double* phases, const size_t* indices, size_t n);

    // Get estimated phase error per sample (radians)
    // This is the residual CFO expressed as phase change per sample
    double get_phase_error_per_sample() const;

    // Get estimated residual CFO (Hz)
    double get_residual_cfo(double sample_rate_hz) const;

    // Reset tracker state
    void reset();

    // Get symbol count
    size_t get_symbol_count() const {
        return symbol_count_;
    }

private:
    PilotPhaseTrackerConfig config_;

    // Phase history for averaging
    std::vector<double> phase_history_;
    size_t history_idx_;

    // Last known phase (for unwrapping)
    double last_phase_;
    bool have_last_phase_;

    // Current estimate
    double phase_error_per_subcarrier_;

    size_t symbol_count_;

    // Unwrap phase to avoid discontinuities
    double unwrap_phase(double phase);
};

}  // namespace sync
}  // namespace atsc3
