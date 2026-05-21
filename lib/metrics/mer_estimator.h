// mer_estimator.h — Modulation Error Ratio (MER) Estimation
//
// MER calculation from equalized constellation points

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

/// Constellation type for MER calculation
enum class ConstellationType : uint8_t {
    QPSK = 0,
    QAM16 = 1,
    QAM64 = 2,
    QAM256 = 3,
    QAM1024 = 4,
    QAM4096 = 5
};

/// Configuration for MER estimator
struct MerEstimatorConfig {
    size_t averaging_window = 1024;  ///< Number of symbols for averaging
    double min_mer_db = -10.0;       ///< Minimum reportable MER
    double max_mer_db = 50.0;        ///< Maximum reportable MER
};

/// Statistics from MER estimation
struct MerStats {
    double mer_db = 0.0;             ///< Current MER estimate in dB
    double evm_percent = 0.0;        ///< Error Vector Magnitude in percent
    double rms_error = 0.0;          ///< RMS error magnitude
    double reference_power = 0.0;    ///< Reference constellation power
    uint64_t symbols_processed = 0;  ///< Total symbols processed
};

/// MER estimator using decision-directed approach
class MerEstimator {
public:
    MerEstimator();
    explicit MerEstimator(const MerEstimatorConfig& config);

    /// Reset estimator state
    void reset();

    /// Set the constellation type for symbol decisions
    void set_constellation(ConstellationType type);

    /// Process equalized symbols
    /// @param symbols Equalized constellation points
    /// @param num_symbols Number of symbols to process
    void process_symbols(const ATSC3_SAMPLE_T* symbols, size_t num_symbols);

    /// Get current MER estimate in dB
    double get_mer_db() const;

    /// Get EVM as percentage
    double get_evm_percent() const;

    /// Get detailed statistics
    MerStats get_stats() const;

    /// Check if estimate is valid
    bool is_valid() const;

private:
    MerEstimatorConfig config_;
    ConstellationType constellation_type_ = ConstellationType::QPSK;

    // Reference constellation normalization factor
    double constellation_scale_ = 1.0;
    int constellation_order_ = 2;  // sqrt of number of points per dimension

    // Running statistics
    double error_power_sum_ = 0.0;
    double reference_power_sum_ = 0.0;
    size_t sample_count_ = 0;

    // Sliding window
    std::vector<double> error_window_;
    std::vector<double> ref_window_;
    size_t window_index_ = 0;
    bool window_filled_ = false;

    // Accumulated stats
    MerStats stats_;

    /// Make hard decision on symbol and return ideal constellation point
    ATSC3_SAMPLE_T hard_decision(const ATSC3_SAMPLE_T& symbol) const;

    /// Update constellation parameters based on type
    void update_constellation_params();
};

}  // namespace metrics
}  // namespace atsc3
