// snr_estimator.cc — SNR Estimation implementation

#include "snr_estimator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace atsc3 {
namespace metrics {

SnrEstimator::SnrEstimator() : SnrEstimator(SnrEstimatorConfig{}) {}

SnrEstimator::SnrEstimator(const SnrEstimatorConfig& config) : config_(config) {
    signal_window_.resize(config_.averaging_window, 0.0);
    error_window_.resize(config_.averaging_window, 0.0);
    reset();
}

void SnrEstimator::reset() {
    signal_power_sum_ = 0.0;
    error_power_sum_ = 0.0;
    sample_count_ = 0;
    window_index_ = 0;
    window_filled_ = false;
    std::fill(signal_window_.begin(), signal_window_.end(), 0.0);
    std::fill(error_window_.begin(), error_window_.end(), 0.0);
    stats_ = SnrStats{};
}

void SnrEstimator::process_pilots(const ATSC3_SAMPLE_T* received_pilots,
                                  const ATSC3_SAMPLE_T* reference_pilots, size_t num_pilots) {
    if (received_pilots == nullptr || reference_pilots == nullptr || num_pilots == 0) {
        return;
    }

    double symbol_signal_power = 0.0;
    double symbol_error_power = 0.0;

    for (size_t i = 0; i < num_pilots; ++i) {
        // Signal power from reference (known transmitted value)
        double ref_power = std::norm(reference_pilots[i]);
        symbol_signal_power += ref_power;

        // Error is difference between received and reference
        ATSC3_SAMPLE_T error = received_pilots[i] - reference_pilots[i];
        double error_power = std::norm(error);
        symbol_error_power += error_power;
    }

    // Update sliding window
    if (window_filled_) {
        // Remove oldest values from sum
        signal_power_sum_ -= signal_window_[window_index_];
        error_power_sum_ -= error_window_[window_index_];
    }

    // Add new values
    signal_window_[window_index_] = symbol_signal_power;
    error_window_[window_index_] = symbol_error_power;
    signal_power_sum_ += symbol_signal_power;
    error_power_sum_ += symbol_error_power;

    // Advance window
    window_index_ = (window_index_ + 1) % config_.averaging_window;
    if (window_index_ == 0) {
        window_filled_ = true;
    }

    sample_count_++;
    stats_.symbols_processed++;
    stats_.pilots_used += num_pilots;

    // Update statistics
    size_t active_window_size = window_filled_ ? config_.averaging_window : window_index_;
    if (active_window_size > 0 && signal_power_sum_ > 0.0) {
        stats_.signal_power = signal_power_sum_ / static_cast<double>(active_window_size);
        stats_.noise_power = error_power_sum_ / static_cast<double>(active_window_size);
        stats_.error_variance = stats_.noise_power;

        if (stats_.noise_power > 1e-15) {
            double snr_linear = stats_.signal_power / stats_.noise_power;
            stats_.snr_db = 10.0 * std::log10(snr_linear);

            // Clamp to valid range
            stats_.snr_db = std::clamp(stats_.snr_db, config_.min_snr_db, config_.max_snr_db);
        } else {
            stats_.snr_db = config_.max_snr_db;
        }
    }
}

double SnrEstimator::get_snr_db() const {
    return stats_.snr_db;
}

SnrStats SnrEstimator::get_stats() const {
    return stats_;
}

bool SnrEstimator::is_valid() const {
    // Need at least half the window filled for valid estimate
    return sample_count_ >= config_.averaging_window / 2;
}

}  // namespace metrics
}  // namespace atsc3
