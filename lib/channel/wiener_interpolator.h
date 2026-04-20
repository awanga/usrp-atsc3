#pragma once

// Wiener Interpolator — 2D channel estimate interpolation
//
// Applies optimal Wiener filtering to interpolate channel estimates
// from pilot positions to all data subcarriers across time and frequency.
//
// The Wiener filter minimizes MSE given known channel statistics
// (Doppler spread, delay spread). Filter coefficients are pre-computed
// for typical channel profiles.
//
// AXI4-S Interface Contract:
//   Input:  Sparse channel estimates at pilot positions
//   Output: Dense channel estimate for all active subcarriers
//
// Reference: Edfors et al., "OFDM Channel Estimation by Singular Value
//            Decomposition", IEEE Trans. Comm. 1998

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace channel {

// Channel profile for Wiener filter design
enum class ChannelProfile {
    AWGN,           // No multipath, no Doppler
    PEDESTRIAN,     // Low Doppler (~5 Hz), short delay spread
    VEHICULAR,      // Medium Doppler (~50 Hz), medium delay spread
    URBAN,          // High multipath, variable Doppler
    STATIC_MULTIPATH // No Doppler, significant delay spread
};

// Wiener interpolator configuration
struct WienerInterpolatorConfig {
    // FFT size
    size_t fft_size = 8192;

    // Number of active carriers
    size_t num_active_carriers = 6913;

    // Filter dimensions
    size_t freq_taps = 8;   // Number of taps in frequency direction
    size_t time_taps = 4;   // Number of taps in time direction (symbols)

    // Channel profile for filter coefficient selection
    ChannelProfile profile = ChannelProfile::PEDESTRIAN;

    // Noise variance estimate (for MMSE filtering)
    // If 0, uses LS (zero-forcing) interpolation
    float noise_variance = 0.01f;

    // Pilot spacing (from pilot pattern)
    size_t pilot_spacing_freq = 6;   // Dx from pilot pattern
    size_t pilot_spacing_time = 4;   // Dy from pilot pattern
};

// Wiener Interpolator
//
// Performs 2D Wiener interpolation of channel estimates from pilot
// positions to all data subcarriers. Uses pre-computed filter coefficients
// optimized for different channel conditions.
//
// Usage:
//   WienerInterpolatorConfig config;
//   config.fft_size = 8192;
//   config.freq_taps = 8;
//   WienerInterpolator interp(config);
//
//   // Interpolate from pilot estimates
//   std::vector<sample_t> pilot_h = estimator.estimate_at_pilots(pilots);
//   std::vector<size_t> pilot_positions = get_pilot_positions(pilots);
//   std::vector<sample_t> full_h = interp.interpolate(pilot_h, pilot_positions);
//
class WienerInterpolator {
public:
    explicit WienerInterpolator(const WienerInterpolatorConfig& config = WienerInterpolatorConfig());
    ~WienerInterpolator();

    // Interpolate channel estimate to all active subcarriers
    // pilot_estimates: LS channel estimates at pilot positions
    // pilot_positions: Subcarrier indices of pilots (relative to first active)
    // Returns: Interpolated channel estimate for all active subcarriers
    std::vector<sample_t> interpolate(
        const std::vector<sample_t>& pilot_estimates,
        const std::vector<size_t>& pilot_positions) const;

    // Interpolate using history buffer (for time-direction filtering)
    // symbol_index: Current OFDM symbol index
    // Updates internal history and uses time-direction filtering
    std::vector<sample_t> interpolate_with_history(
        const std::vector<sample_t>& pilot_estimates,
        const std::vector<size_t>& pilot_positions,
        size_t symbol_index);

    // Reset history buffer
    void reset();

    // Update configuration
    void set_config(const WienerInterpolatorConfig& config);
    const WienerInterpolatorConfig& get_config() const { return config_; }

    // Get filter coefficients (for debugging/analysis)
    const std::vector<float>& get_freq_coefficients() const { return freq_coeffs_; }
    const std::vector<float>& get_time_coefficients() const { return time_coeffs_; }

private:
    WienerInterpolatorConfig config_;

    // Pre-computed filter coefficients
    std::vector<float> freq_coeffs_;  // Frequency-direction filter
    std::vector<float> time_coeffs_;  // Time-direction filter

    // History buffer for time-direction filtering
    // Stores previous symbol estimates [symbol][subcarrier]
    std::vector<std::vector<sample_t>> history_;
    size_t history_write_idx_ = 0;
    bool history_filled_ = false;

    // Initialize filter coefficients
    void init();
    void compute_filter_coefficients();

    // Frequency-direction interpolation (within one symbol)
    void interpolate_frequency(
        const std::vector<sample_t>& pilot_estimates,
        const std::vector<size_t>& pilot_positions,
        std::vector<sample_t>& output) const;

    // Time-direction filtering (across symbols)
    void filter_time(std::vector<sample_t>& estimate);

    // Apply FIR filter at a single position
    sample_t apply_fir(const std::vector<sample_t>& input,
                       size_t center_idx,
                       const std::vector<float>& coeffs) const;
};

// Utility: Compute Wiener filter coefficients for given channel statistics
// doppler_hz: Maximum Doppler spread
// delay_spread_us: RMS delay spread in microseconds
// snr_db: Operating SNR
// num_taps: Number of filter taps
std::vector<float> compute_wiener_coefficients(
    float doppler_hz,
    float delay_spread_us,
    float snr_db,
    size_t num_taps);

}  // namespace channel
}  // namespace atsc3
