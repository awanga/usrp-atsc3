// metrics_aggregator.h — Combined Signal Quality Metrics
//
// Aggregates all metrics into JSON output format

#pragma once

#include "ber_estimator.h"
#include "mer_estimator.h"
#include "signal_strength.h"
#include "snr_estimator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atsc3 {
namespace metrics {

/// Service information for metrics output
struct ServiceInfo {
    uint16_t service_id = 0;
    std::array<char, 64> name = {};
    bool active = false;
};

/// Aggregated metrics snapshot
struct MetricsSnapshot {
    // Signal quality
    double rssi_dbm = -120.0;
    double snr_db = 0.0;
    double mer_db = 0.0;
    double evm_percent = 0.0;

    // FEC performance
    double ber_pre_fec = 0.0;
    double fer = 0.0;
    bool ldpc_converged = false;
    double avg_ldpc_iterations = 0.0;

    // Lock status
    bool signal_locked = false;
    bool frame_locked = false;

    // Timing
    uint64_t timestamp_ms = 0;
    uint64_t uptime_ms = 0;

    // Services
    std::vector<ServiceInfo> services;
};

/// Configuration for metrics aggregator
struct MetricsAggregatorConfig {
    SnrEstimatorConfig snr_config;
    MerEstimatorConfig mer_config;
    BerEstimatorConfig ber_config;
    SignalStrengthConfig signal_config;
};

/// Aggregates all signal quality metrics
class MetricsAggregator {
public:
    MetricsAggregator();
    explicit MetricsAggregator(const MetricsAggregatorConfig& config);

    /// Reset all metrics
    void reset();

    /// Access individual estimators for updates
    SnrEstimator& snr_estimator() {
        return snr_estimator_;
    }
    MerEstimator& mer_estimator() {
        return mer_estimator_;
    }
    BerEstimator& ber_estimator() {
        return ber_estimator_;
    }
    SignalStrength& signal_strength() {
        return signal_strength_;
    }

    /// Update lock status
    void set_signal_locked(bool locked);
    void set_frame_locked(bool locked);

    /// Update service list
    void update_services(const std::vector<ServiceInfo>& services);

    /// Get current metrics snapshot
    MetricsSnapshot get_snapshot() const;

    /// Get metrics as JSON string
    std::string to_json() const;

    /// Get metrics as compact JSON (single line)
    std::string to_json_compact() const;

private:
    SnrEstimator snr_estimator_;
    MerEstimator mer_estimator_;
    BerEstimator ber_estimator_;
    SignalStrength signal_strength_;

    bool signal_locked_ = false;
    bool frame_locked_ = false;
    uint64_t start_time_ms_ = 0;

    std::vector<ServiceInfo> services_;

    /// Get current timestamp in milliseconds
    static uint64_t get_timestamp_ms();
};

}  // namespace metrics
}  // namespace atsc3
