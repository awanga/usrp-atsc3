#pragma once

// AGC Controller — Automatic Gain Control for IQ sources
//
// Power-feedback AGC that adjusts gain to maintain target power level.
// Uses integrator with configurable attack/release time constants.

#include <complex>
#include <cstddef>

namespace atsc3 {
namespace hal {

// AGC configuration parameters
struct AgcConfig {
    // Target power in dBFS (dB relative to full scale)
    // Default: -20 dBFS (allows for 20 dB headroom for peaks)
    double target_power_dbfs = -20.0;

    // Attack time constant in samples
    // Controls how quickly gain decreases when signal is too strong
    // Default: 1000 samples (~0.16 ms at 6.25 MS/s)
    double attack_samples = 1000.0;

    // Release time constant in samples
    // Controls how slowly gain increases when signal is too weak
    // Default: 10000 samples (~1.6 ms at 6.25 MS/s)
    // Release is typically slower than attack to avoid pumping
    double release_samples = 10000.0;

    // Gain limits in dB
    double min_gain_db = 0.0;
    double max_gain_db = 95.0;  // TVRX maximum

    // Initial gain in dB
    double initial_gain_db = 30.0;

    // Deadband: don't adjust gain if power is within this range of target
    // Prevents hunting around the setpoint
    double deadband_db = 1.0;
};

// AGC Controller
//
// Usage:
//   AgcController agc(config);
//   // In processing loop:
//   double new_gain = agc.process(buffer, num_samples);
//   if (new_gain != current_gain) {
//       source->set_gain(new_gain);
//       current_gain = new_gain;
//   }
//
class AgcController {
public:
    explicit AgcController(const AgcConfig& config = AgcConfig());

    // Process a buffer of samples and compute new gain setting
    // Returns the recommended gain in dB
    //
    // The AGC measures the power of the input buffer and adjusts
    // the internal gain estimate using a leaky integrator.
    //
    // This method does NOT modify the input samples or apply the gain.
    // The caller is responsible for setting the source gain.
    double process(const std::complex<float>* buf, size_t n);

    // Get current gain setting in dB
    double get_gain() const;

    // Get current measured power in dBFS
    double get_power_dbfs() const;

    // Reset AGC state to initial values
    void reset();

    // Update configuration
    void set_config(const AgcConfig& config);

    // Get current configuration
    const AgcConfig& get_config() const;

private:
    AgcConfig config_;

    // Current state
    double current_gain_db_;
    double measured_power_dbfs_;

    // Precomputed coefficients
    double attack_coeff_;
    double release_coeff_;

    // Compute filter coefficients from time constants
    void update_coefficients();

    // Compute power in dBFS from sample buffer
    static double compute_power_dbfs(const std::complex<float>* buf, size_t n);
};

}  // namespace hal
}  // namespace atsc3
