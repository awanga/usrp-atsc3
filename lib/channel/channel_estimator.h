#pragma once

// Channel Estimator — ATSC 3.0 OFDM channel estimation
//
// Computes channel frequency response H(f) from pilot observations.
// Supports Least-Squares (LS) estimation at pilot positions with
// interpolation to data subcarriers.
//
// AXI4-S Interface Contract:
//   Input:  Vector of PilotSymbol from pilot_extractor
//   Output: Vector of sample_t (channel estimate for all active subcarriers)
//
// Reference: ATSC A/322 Section 7, Edfors et al. IEEE Trans. Comm. 1998

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Forward declaration to avoid including full pilot_extractor header
namespace atsc3 {
namespace ofdm {
struct PilotSymbol;
}
}  // namespace atsc3

namespace atsc3 {
namespace channel {

// Estimation backend selection
// LS_ONLY:  Traditional Least-Squares (production default)
// LS_WIENER: LS + Wiener interpolation (better for multipath)
// ML_ONNX:  ML-based estimator via ONNX Runtime (post-MVP)
enum class EstimatorBackend {
    LS_ONLY,    // LS at pilots, linear interpolation to data
    LS_WIENER,  // LS at pilots, Wiener filter interpolation
    ML_ONNX     // ML model inference (post-MVP stub)
};

// Interpolation method for filling gaps between pilots
enum class InterpolationMethod {
    LINEAR,      // Simple linear interpolation
    CUBIC,       // Cubic spline interpolation
    WIENER       // Optimal Wiener filter (requires noise estimate)
};

// Channel estimator configuration
struct ChannelEstimatorConfig {
    // FFT size
    size_t fft_size = 8192;

    // Active carrier range
    size_t first_active_carrier = 0;  // 0 = auto-compute
    size_t num_active_carriers = 0;   // 0 = auto-compute

    // Backend selection
    EstimatorBackend backend = EstimatorBackend::LS_ONLY;

    // Interpolation method (for LS backends)
    InterpolationMethod interpolation = InterpolationMethod::LINEAR;

    // Noise variance estimate (for MMSE/Wiener filtering)
    // Set to 0 for pure LS (ZF) estimation
    float noise_variance = 0.0f;

    // Enable averaging across symbols (temporal smoothing)
    bool enable_averaging = false;
    float averaging_alpha = 0.1f;  // IIR filter coefficient

    // ONNX model path (for ML_ONNX backend, post-MVP)
    // std::string onnx_model_path;
};

// Channel estimate output for a single symbol
struct ChannelEstimate {
    // Full channel estimate for all active subcarriers
    std::vector<sample_t> h_estimate;

    // Estimated SNR from pilot residuals (dB)
    float estimated_snr_db = 0.0f;

    // Mean phase error (radians)
    float mean_phase_error = 0.0f;

    // Valid flag
    bool is_valid = false;

    // Symbol index this estimate corresponds to
    size_t symbol_index = 0;
};

// Channel Estimator
//
// Estimates the channel frequency response H(f) from pilot symbols.
// Primary algorithm is Least-Squares (LS) at pilot positions followed
// by interpolation to data subcarriers.
//
// Usage:
//   ChannelEstimatorConfig config;
//   config.fft_size = 8192;
//   ChannelEstimator estimator(config);
//
//   // Estimate from pilot observations
//   std::vector<PilotSymbol> pilots = extractor.extract(fft_output, sym_idx);
//   ChannelEstimate h = estimator.estimate(pilots, sym_idx);
//
//   // Equalize data symbols
//   for (size_t k = 0; k < num_active; ++k) {
//       equalized[k] = fft_output[k] / h.h_estimate[k];
//   }
//
class ChannelEstimator {
public:
    explicit ChannelEstimator(const ChannelEstimatorConfig& config = ChannelEstimatorConfig());
    ~ChannelEstimator();

    // Estimate channel from pilot observations
    // pilots: Vector of pilot symbols from PilotExtractor
    // symbol_index: OFDM symbol index
    // Returns: Channel estimate for all active subcarriers
    ChannelEstimate estimate(const std::vector<ofdm::PilotSymbol>& pilots,
                              size_t symbol_index);

    // Estimate channel at pilot positions only (no interpolation)
    // Returns: Channel estimate only at pilot subcarrier positions
    std::vector<sample_t> estimate_at_pilots(
        const std::vector<ofdm::PilotSymbol>& pilots) const;

    // Get estimated SNR from most recent estimation
    float get_estimated_snr_db() const { return last_snr_db_; }

    // Reset internal state (averaging buffers, etc.)
    void reset();

    // Update configuration
    void set_config(const ChannelEstimatorConfig& config);
    const ChannelEstimatorConfig& get_config() const { return config_; }

    // Set backend (runtime switching)
    void set_backend(EstimatorBackend backend);
    EstimatorBackend get_backend() const { return config_.backend; }

    // Get number of active carriers
    size_t get_num_active_carriers() const;

    // Get first active carrier index
    size_t get_first_active_carrier() const;

private:
    ChannelEstimatorConfig config_;

    // Averaging buffer for temporal smoothing
    std::vector<sample_t> avg_estimate_;
    bool has_prev_estimate_ = false;

    // Last computed SNR
    float last_snr_db_ = 0.0f;

    // Initialize from config
    void init();

    // Compute LS estimate at pilot positions
    // H_hat[k] = Y[k] / X[k] where Y is received, X is reference
    std::vector<sample_t> compute_ls_estimate(
        const std::vector<ofdm::PilotSymbol>& pilots) const;

    // Interpolate channel estimate to all active subcarriers
    void interpolate(const std::vector<sample_t>& pilot_estimates,
                     const std::vector<size_t>& pilot_positions,
                     std::vector<sample_t>& full_estimate) const;

    // Linear interpolation
    void interpolate_linear(const std::vector<sample_t>& pilot_estimates,
                            const std::vector<size_t>& pilot_positions,
                            std::vector<sample_t>& full_estimate) const;

    // Apply temporal averaging
    void apply_averaging(std::vector<sample_t>& estimate);

    // Estimate SNR from pilot residuals
    float estimate_snr(const std::vector<ofdm::PilotSymbol>& pilots,
                       const std::vector<sample_t>& h_estimates) const;

    // Compute mean phase error from pilots
    float compute_mean_phase_error(
        const std::vector<ofdm::PilotSymbol>& pilots) const;
};

// Utility: Complex division with saturation for fixed-point
// Returns a / b with proper handling of small b values
sample_t safe_complex_divide(sample_t a, sample_t b);

}  // namespace channel
}  // namespace atsc3
