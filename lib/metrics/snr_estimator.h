// snr_estimator.h — SNR Estimation from pilot residuals
//
// Decision-directed SNR estimation using scattered pilots

#pragma once

#include "types.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace metrics {

/// Configuration for SNR estimator
struct SnrEstimatorConfig {
    size_t averaging_window = 64;      ///< Number of symbols for averaging
    double min_snr_db = -10.0;         ///< Minimum reportable SNR
    double max_snr_db = 50.0;          ///< Maximum reportable SNR
    bool use_continual_pilots = true;  ///< Include continual pilots in estimate
    bool use_scattered_pilots = true;  ///< Include scattered pilots in estimate
};

/// Statistics from SNR estimation
struct SnrStats {
    double snr_db = 0.0;             ///< Current SNR estimate in dB
    double signal_power = 0.0;       ///< Estimated signal power (linear)
    double noise_power = 0.0;        ///< Estimated noise power (linear)
    double error_variance = 0.0;     ///< Pilot error variance
    uint64_t symbols_processed = 0;  ///< Total symbols processed
    uint64_t pilots_used = 0;        ///< Total pilots used in estimate
};

/// SNR estimator using pilot-based error measurement
class SnrEstimator {
public:
    SnrEstimator();
    explicit SnrEstimator(const SnrEstimatorConfig& config);

    /// Reset estimator state
    void reset();

    /// Process pilot observations from one OFDM symbol
    /// @param received_pilots Received pilot values
    /// @param reference_pilots Known transmitted pilot values
    /// @param num_pilots Number of pilots in this symbol
    void process_pilots(const ATSC3_SAMPLE_T* received_pilots,
                        const ATSC3_SAMPLE_T* reference_pilots, size_t num_pilots);

    /// Get current SNR estimate in dB
    double get_snr_db() const;

    /// Get detailed statistics
    SnrStats get_stats() const;

    /// Check if estimate is valid (enough samples processed)
    bool is_valid() const;

private:
    SnrEstimatorConfig config_;

    // Running statistics
    double signal_power_sum_ = 0.0;
    double error_power_sum_ = 0.0;
    size_t sample_count_ = 0;

    // Sliding window for averaging
    std::vector<double> signal_window_;
    std::vector<double> error_window_;
    size_t window_index_ = 0;
    bool window_filled_ = false;

    // Accumulated stats
    SnrStats stats_;
};

}  // namespace metrics
}  // namespace atsc3
