// ber_estimator.h — Pre-FEC BER Estimation
//
// BER proxy estimation from LDPC decoder iteration count

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace metrics {

/// Configuration for BER estimator
struct BerEstimatorConfig {
    size_t averaging_window = 100;  ///< Number of codewords for averaging
    int max_ldpc_iterations = 50;   ///< Maximum LDPC iterations (from decoder config)
    double ber_floor = 1e-10;       ///< Minimum reportable BER
    double ber_ceiling = 0.5;       ///< Maximum reportable BER
};

/// Statistics from BER estimation
struct BerStats {
    double ber_estimate = 0.0;         ///< Estimated pre-FEC BER
    double fer_estimate = 0.0;         ///< Frame Error Rate
    double avg_iterations = 0.0;       ///< Average LDPC iterations
    uint64_t codewords_processed = 0;  ///< Total codewords
    uint64_t codewords_converged = 0;  ///< Codewords that converged
    uint64_t codewords_failed = 0;     ///< Codewords that failed
};

/// LDPC decode result for BER estimation
struct LdpcResult {
    int iterations_used;  ///< Number of iterations taken
    bool converged;       ///< Whether decoder converged
    int max_iterations;   ///< Maximum iterations allowed
};

/// BER estimator using LDPC iteration count as proxy
///
/// The number of LDPC iterations correlates with input BER:
/// - Fewer iterations = cleaner signal = lower BER
/// - More iterations = noisier signal = higher BER
/// - Non-convergence = very high BER
class BerEstimator {
public:
    BerEstimator();
    explicit BerEstimator(const BerEstimatorConfig& config);

    /// Reset estimator state
    void reset();

    /// Process LDPC decode result
    void process_codeword(const LdpcResult& result);

    /// Process LDPC decode result (convenience overload)
    void process_codeword(int iterations_used, bool converged, int max_iterations);

    /// Get estimated pre-FEC BER
    double get_ber() const;

    /// Get Frame Error Rate
    double get_fer() const;

    /// Get detailed statistics
    BerStats get_stats() const;

    /// Check if estimate is valid
    bool is_valid() const;

private:
    BerEstimatorConfig config_;

    // Sliding window for iteration counts
    std::vector<int> iteration_window_;
    std::vector<bool> converged_window_;
    size_t window_index_ = 0;
    bool window_filled_ = false;

    // Running sums
    int64_t iteration_sum_ = 0;
    size_t converged_count_ = 0;
    size_t sample_count_ = 0;

    // Accumulated stats
    BerStats stats_;

    /// Map iteration count to BER estimate
    double iterations_to_ber(double avg_iterations, double convergence_rate) const;
};

}  // namespace metrics
}  // namespace atsc3
