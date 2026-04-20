#pragma once

// Frequency-Domain Equalizer — ATSC 3.0 OFDM equalization
//
// Performs single-tap frequency-domain equalization (FDE) to recover
// transmitted QAM symbols from received FFT output using channel estimates.
//
// Supports both Zero-Forcing (ZF) and Minimum Mean Square Error (MMSE)
// equalization modes. Includes residual phase noise tracking from
// continual pilots.
//
// AXI4-S Interface Contract:
//   Input:  Vector of sample_t (FFT output for one OFDM symbol)
//           Vector of sample_t (channel estimate from ChannelEstimator)
//   Output: Vector of sample_t (equalized QAM symbols)
//
// Reference: ATSC A/322 Section 7

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward declaration
namespace atsc3 {
namespace ofdm {
struct PilotSymbol;
}
}  // namespace atsc3

namespace atsc3 {
namespace channel {

// Equalization mode
enum class EqualizationMode {
    ZERO_FORCING,  // ZF: X_hat = Y / H
    MMSE           // MMSE: X_hat = Y * conj(H) / (|H|² + σ²_n)
};

// Equalizer configuration
struct EqualizerConfig {
    // FFT size
    size_t fft_size = 8192;

    // Active carrier range
    size_t first_active_carrier = 0;  // 0 = auto-compute
    size_t num_active_carriers = 0;   // 0 = auto-compute

    // Equalization mode
    EqualizationMode mode = EqualizationMode::ZERO_FORCING;

    // Noise variance estimate for MMSE equalization
    // Higher values = more regularization = less noise enhancement
    float noise_variance = 0.01f;

    // Phase noise tracking
    bool enable_phase_tracking = true;

    // Phase tracking IIR filter coefficient (0 = no smoothing, 1 = no update)
    float phase_tracking_alpha = 0.3f;

    // Minimum channel magnitude for equalization
    // Subcarriers with |H| below this are zeroed to avoid noise blow-up
    float min_channel_magnitude = 0.01f;
};

// Equalization result for a single OFDM symbol
struct EqualizationResult {
    // Equalized symbols for all active subcarriers
    std::vector<sample_t> symbols;

    // Estimated residual phase (radians)
    float residual_phase = 0.0f;

    // Number of subcarriers that were zeroed due to deep fades
    size_t num_zeroed_carriers = 0;

    // Estimated EVM from equalization (if pilots available)
    float estimated_evm_db = 0.0f;

    // Valid flag
    bool is_valid = false;

    // Symbol index
    size_t symbol_index = 0;
};

// Frequency-Domain Equalizer
//
// Equalizes received OFDM symbols using channel estimates from
// the ChannelEstimator. Supports ZF and MMSE modes.
//
// Usage:
//   EqualizerConfig config;
//   config.mode = EqualizationMode::MMSE;
//   Equalizer equalizer(config);
//
//   // Get channel estimate
//   ChannelEstimate h = estimator.estimate(pilots, sym_idx);
//
//   // Equalize received symbols
//   EqualizationResult result = equalizer.equalize(
//       fft_output.data() + first_active,
//       h.h_estimate,
//       sym_idx);
//
class Equalizer {
public:
    explicit Equalizer(const EqualizerConfig& config = EqualizerConfig());
    ~Equalizer();

    // Equalize received symbols
    // received: FFT output for active subcarriers (num_active_carriers samples)
    // channel_estimate: Channel estimate H for all active subcarriers
    // symbol_index: OFDM symbol index
    // Returns: Equalized QAM symbols
    EqualizationResult equalize(
        const sample_t* received,
        const std::vector<sample_t>& channel_estimate,
        size_t symbol_index);

    // Equalize with explicit pilot symbols for phase tracking
    // pilots: Continual pilot observations for residual phase estimation
    EqualizationResult equalize_with_pilots(
        const sample_t* received,
        const std::vector<sample_t>& channel_estimate,
        const std::vector<ofdm::PilotSymbol>& pilots,
        size_t symbol_index);

    // Equalize a single subcarrier (for external use)
    // y: Received symbol
    // h: Channel estimate
    // Returns: Equalized symbol
    sample_t equalize_sample(sample_t y, sample_t h) const;

    // Get estimated residual phase from last equalization
    float get_residual_phase() const { return residual_phase_; }

    // Reset internal state
    void reset();

    // Update configuration
    void set_config(const EqualizerConfig& config);
    const EqualizerConfig& get_config() const { return config_; }

    // Get number of active carriers
    size_t get_num_active_carriers() const;

    // Get first active carrier index
    size_t get_first_active_carrier() const;

private:
    EqualizerConfig config_;

    // Tracked residual phase (smoothed)
    float residual_phase_ = 0.0f;

    // Phase rotation phasor for efficient application
    sample_t phase_phasor_ = sample_t(1.0f, 0.0f);

    // Initialize from config
    void init();

    // Zero-forcing equalization
    sample_t equalize_zf(sample_t y, sample_t h) const;

    // MMSE equalization
    sample_t equalize_mmse(sample_t y, sample_t h) const;

    // Estimate residual phase from continual pilots
    float estimate_residual_phase(
        const sample_t* received,
        const std::vector<sample_t>& channel_estimate,
        const std::vector<ofdm::PilotSymbol>& pilots) const;

    // Apply phase correction to output symbols
    void apply_phase_correction(std::vector<sample_t>& symbols, float phase);

    // Update phase phasor from phase angle
    void update_phase_phasor(float phase);
};

// Compute EVM in dB from equalized symbols and reference
// equalized: Equalized QAM symbols
// reference: Ideal constellation points
// Returns: EVM in dB (20 * log10(rms_error / rms_signal))
float compute_evm_db(const std::vector<sample_t>& equalized,
                     const std::vector<sample_t>& reference);

}  // namespace channel
}  // namespace atsc3
