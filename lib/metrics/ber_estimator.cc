// ber_estimator.cc — BER Estimation implementation

#include "ber_estimator.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace metrics {

BerEstimator::BerEstimator() : BerEstimator(BerEstimatorConfig{}) {}

BerEstimator::BerEstimator(const BerEstimatorConfig& config) : config_(config) {
    iteration_window_.resize(config_.averaging_window, 0);
    converged_window_.resize(config_.averaging_window, true);
    reset();
}

void BerEstimator::reset() {
    iteration_sum_ = 0;
    converged_count_ = 0;
    sample_count_ = 0;
    window_index_ = 0;
    window_filled_ = false;
    std::fill(iteration_window_.begin(), iteration_window_.end(), 0);
    std::fill(converged_window_.begin(), converged_window_.end(), true);
    stats_ = BerStats{};
}

void BerEstimator::process_codeword(const LdpcResult& result) {
    process_codeword(result.iterations_used, result.converged, result.max_iterations);
}

void BerEstimator::process_codeword(int iterations_used, bool converged, int max_iterations) {
    // Update sliding window
    if (window_filled_) {
        iteration_sum_ -= iteration_window_[window_index_];
        if (converged_window_[window_index_]) {
            converged_count_--;
        }
    }

    // Clamp iterations to valid range
    iterations_used = std::clamp(iterations_used, 1, max_iterations);

    iteration_window_[window_index_] = iterations_used;
    converged_window_[window_index_] = converged;
    iteration_sum_ += iterations_used;
    if (converged) {
        converged_count_++;
    }

    window_index_ = (window_index_ + 1) % config_.averaging_window;
    if (window_index_ == 0) {
        window_filled_ = true;
    }

    sample_count_++;
    stats_.codewords_processed++;
    if (converged) {
        stats_.codewords_converged++;
    } else {
        stats_.codewords_failed++;
    }

    // Calculate statistics
    size_t active_size = window_filled_ ? config_.averaging_window : window_index_;
    if (active_size > 0) {
        stats_.avg_iterations =
            static_cast<double>(iteration_sum_) / static_cast<double>(active_size);

        double convergence_rate =
            static_cast<double>(converged_count_) / static_cast<double>(active_size);
        stats_.fer_estimate = 1.0 - convergence_rate;

        stats_.ber_estimate = iterations_to_ber(stats_.avg_iterations, convergence_rate);
    }
}

double BerEstimator::iterations_to_ber(double avg_iterations, double convergence_rate) const {
    // Empirical mapping from LDPC iterations to BER
    // Based on typical LDPC waterfall characteristics:
    // - 1-5 iterations: very clean signal, BER < 1e-6
    // - 5-20 iterations: good signal, BER ~1e-4 to 1e-6
    // - 20-40 iterations: marginal signal, BER ~1e-2 to 1e-4
    // - 40+ iterations or non-convergence: poor signal, BER > 1e-2

    // First, handle non-convergence
    if (convergence_rate < 0.5) {
        // More than half failed - very high BER
        return std::min(0.1 * (1.0 - convergence_rate) + 0.01, config_.ber_ceiling);
    }

    // Normalize iterations to max
    double normalized = avg_iterations / static_cast<double>(config_.max_ldpc_iterations);

    // Piecewise linear mapping (empirical)
    double ber;
    if (normalized < 0.1) {
        // Very few iterations - excellent signal
        ber = 1e-8 + normalized * 1e-5;
    } else if (normalized < 0.3) {
        // Few iterations - good signal
        ber = 1e-6 + (normalized - 0.1) * 5e-4;
    } else if (normalized < 0.6) {
        // Moderate iterations - acceptable signal
        ber = 1e-4 + (normalized - 0.3) * 3e-3;
    } else if (normalized < 0.9) {
        // Many iterations - marginal signal
        ber = 1e-3 + (normalized - 0.6) * 3e-2;
    } else {
        // Near-max iterations - poor signal
        ber = 1e-2 + (normalized - 0.9) * 0.1;
    }

    // Factor in convergence rate
    ber *= (2.0 - convergence_rate);

    return std::clamp(ber, config_.ber_floor, config_.ber_ceiling);
}

double BerEstimator::get_ber() const {
    return stats_.ber_estimate;
}

double BerEstimator::get_fer() const {
    return stats_.fer_estimate;
}

BerStats BerEstimator::get_stats() const {
    return stats_;
}

bool BerEstimator::is_valid() const {
    return sample_count_ >= config_.averaging_window / 2;
}

}  // namespace metrics
}  // namespace atsc3
