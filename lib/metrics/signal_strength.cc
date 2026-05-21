// signal_strength.cc — Signal Strength implementation

#include "signal_strength.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atsc3 {
namespace metrics {

SignalStrength::SignalStrength() : SignalStrength(SignalStrengthConfig{}) {}

SignalStrength::SignalStrength(const SignalStrengthConfig& config) : config_(config) {
    rssi_window_ = new double[config_.averaging_samples];
    reset();
}

SignalStrength::~SignalStrength() {
    delete[] rssi_window_;
}

void SignalStrength::reset() {
    rssi_sum_ = 0.0;
    window_index_ = 0;
    window_filled_ = false;

    for (size_t i = 0; i < config_.averaging_samples; ++i) {
        rssi_window_[i] = config_.min_rssi_dbm;
    }

    current_rssi_dbm_ = config_.min_rssi_dbm;
    current_agc_gain_db_ = 0.0;

    stats_ = SignalStrengthStats{};
    stats_.rssi_dbm_min = config_.max_rssi_dbm;
    stats_.rssi_dbm_peak = config_.min_rssi_dbm;
}

void SignalStrength::update_rssi(double rssi_dbm) {
    // Clamp to valid range
    rssi_dbm = std::clamp(rssi_dbm, config_.min_rssi_dbm, config_.max_rssi_dbm);

    current_rssi_dbm_ = rssi_dbm;

    // Update sliding window average
    if (window_filled_) {
        rssi_sum_ -= rssi_window_[window_index_];
    }

    rssi_window_[window_index_] = rssi_dbm;
    rssi_sum_ += rssi_dbm;

    window_index_ = (window_index_ + 1) % config_.averaging_samples;
    if (window_index_ == 0) {
        window_filled_ = true;
    }

    // Update statistics
    stats_.samples_processed++;
    stats_.rssi_dbm = rssi_dbm;

    size_t active_size = window_filled_ ? config_.averaging_samples : window_index_;
    if (active_size > 0) {
        stats_.rssi_dbm_avg = rssi_sum_ / static_cast<double>(active_size);
    }

    stats_.rssi_dbm_peak = std::max(stats_.rssi_dbm_peak, rssi_dbm);
    stats_.rssi_dbm_min = std::min(stats_.rssi_dbm_min, rssi_dbm);

    // Calculate corrected RSSI
    stats_.corrected_rssi_dbm =
        rssi_dbm - current_agc_gain_db_ + config_.agc_gain_offset_db + config_.rssi_calibration_db;
}

void SignalStrength::update_agc_gain(double gain_db) {
    current_agc_gain_db_ = gain_db;
    stats_.agc_gain_db = gain_db;

    // Recalculate corrected RSSI
    stats_.corrected_rssi_dbm = current_rssi_dbm_ - current_agc_gain_db_ +
                                config_.agc_gain_offset_db + config_.rssi_calibration_db;
}

double SignalStrength::get_rssi_dbm() const {
    return current_rssi_dbm_;
}

double SignalStrength::get_corrected_rssi_dbm() const {
    return stats_.corrected_rssi_dbm;
}

SignalStrengthStats SignalStrength::get_stats() const {
    return stats_;
}

bool SignalStrength::is_valid() const {
    return stats_.samples_processed > 0;
}

}  // namespace metrics
}  // namespace atsc3
