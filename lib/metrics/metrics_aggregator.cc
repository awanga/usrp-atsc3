// metrics_aggregator.cc — Metrics Aggregator implementation

#include "metrics_aggregator.h"

#include <chrono>
#include <cstdio>
#include <sstream>

namespace atsc3 {
namespace metrics {

MetricsAggregator::MetricsAggregator() : MetricsAggregator(MetricsAggregatorConfig{}) {}

MetricsAggregator::MetricsAggregator(const MetricsAggregatorConfig& config)
    : snr_estimator_(config.snr_config),
      mer_estimator_(config.mer_config),
      ber_estimator_(config.ber_config),
      signal_strength_(config.signal_config) {
    start_time_ms_ = get_timestamp_ms();
}

void MetricsAggregator::reset() {
    snr_estimator_.reset();
    mer_estimator_.reset();
    ber_estimator_.reset();
    signal_strength_.reset();
    signal_locked_ = false;
    frame_locked_ = false;
    services_.clear();
    start_time_ms_ = get_timestamp_ms();
}

void MetricsAggregator::set_signal_locked(bool locked) {
    signal_locked_ = locked;
}

void MetricsAggregator::set_frame_locked(bool locked) {
    frame_locked_ = locked;
}

void MetricsAggregator::update_services(const std::vector<ServiceInfo>& services) {
    services_ = services;
}

MetricsSnapshot MetricsAggregator::get_snapshot() const {
    MetricsSnapshot snapshot;

    // Signal strength
    auto sig_stats = signal_strength_.get_stats();
    snapshot.rssi_dbm = sig_stats.corrected_rssi_dbm;

    // SNR
    auto snr_stats = snr_estimator_.get_stats();
    snapshot.snr_db = snr_stats.snr_db;

    // MER
    auto mer_stats = mer_estimator_.get_stats();
    snapshot.mer_db = mer_stats.mer_db;
    snapshot.evm_percent = mer_stats.evm_percent;

    // BER/FER
    auto ber_stats = ber_estimator_.get_stats();
    snapshot.ber_pre_fec = ber_stats.ber_estimate;
    snapshot.fer = ber_stats.fer_estimate;
    snapshot.avg_ldpc_iterations = ber_stats.avg_iterations;
    snapshot.ldpc_converged = ber_stats.codewords_failed == 0 ||
                              (ber_stats.codewords_converged > ber_stats.codewords_failed * 10);

    // Lock status
    snapshot.signal_locked = signal_locked_;
    snapshot.frame_locked = frame_locked_;

    // Timing
    snapshot.timestamp_ms = get_timestamp_ms();
    snapshot.uptime_ms = snapshot.timestamp_ms - start_time_ms_;

    // Services
    snapshot.services = services_;

    return snapshot;
}

std::string MetricsAggregator::to_json() const {
    auto snapshot = get_snapshot();
    std::ostringstream oss;

    oss << "{\n";
    oss << "  \"rssi_dbm\": " << snapshot.rssi_dbm << ",\n";
    oss << "  \"snr_db\": " << snapshot.snr_db << ",\n";
    oss << "  \"mer_db\": " << snapshot.mer_db << ",\n";
    oss << "  \"evm_percent\": " << snapshot.evm_percent << ",\n";
    oss << "  \"ber_pre_fec\": " << snapshot.ber_pre_fec << ",\n";
    oss << "  \"fer\": " << snapshot.fer << ",\n";
    oss << "  \"ldpc_converged\": " << (snapshot.ldpc_converged ? "true" : "false") << ",\n";
    oss << "  \"avg_ldpc_iterations\": " << snapshot.avg_ldpc_iterations << ",\n";
    oss << "  \"signal_locked\": " << (snapshot.signal_locked ? "true" : "false") << ",\n";
    oss << "  \"frame_locked\": " << (snapshot.frame_locked ? "true" : "false") << ",\n";
    oss << "  \"timestamp_ms\": " << snapshot.timestamp_ms << ",\n";
    oss << "  \"uptime_ms\": " << snapshot.uptime_ms << ",\n";
    oss << "  \"services\": [";

    for (size_t i = 0; i < snapshot.services.size(); ++i) {
        const auto& svc = snapshot.services[i];
        if (i > 0)
            oss << ",";
        oss << "\n    {\n";
        oss << "      \"service_id\": " << svc.service_id << ",\n";
        oss << "      \"name\": \"" << svc.name.data() << "\",\n";
        oss << "      \"active\": " << (svc.active ? "true" : "false") << "\n";
        oss << "    }";
    }

    if (!snapshot.services.empty()) {
        oss << "\n  ";
    }
    oss << "]\n";
    oss << "}";

    return oss.str();
}

std::string MetricsAggregator::to_json_compact() const {
    auto snapshot = get_snapshot();
    std::ostringstream oss;

    oss << "{";
    oss << "\"rssi_dbm\":" << snapshot.rssi_dbm << ",";
    oss << "\"snr_db\":" << snapshot.snr_db << ",";
    oss << "\"mer_db\":" << snapshot.mer_db << ",";
    oss << "\"evm_percent\":" << snapshot.evm_percent << ",";
    oss << "\"ber_pre_fec\":" << snapshot.ber_pre_fec << ",";
    oss << "\"fer\":" << snapshot.fer << ",";
    oss << "\"ldpc_converged\":" << (snapshot.ldpc_converged ? "true" : "false") << ",";
    oss << "\"avg_ldpc_iterations\":" << snapshot.avg_ldpc_iterations << ",";
    oss << "\"signal_locked\":" << (snapshot.signal_locked ? "true" : "false") << ",";
    oss << "\"frame_locked\":" << (snapshot.frame_locked ? "true" : "false") << ",";
    oss << "\"timestamp_ms\":" << snapshot.timestamp_ms << ",";
    oss << "\"uptime_ms\":" << snapshot.uptime_ms << ",";
    oss << "\"services\":[";

    for (size_t i = 0; i < snapshot.services.size(); ++i) {
        const auto& svc = snapshot.services[i];
        if (i > 0)
            oss << ",";
        oss << "{\"service_id\":" << svc.service_id << ",";
        oss << "\"name\":\"" << svc.name.data() << "\",";
        oss << "\"active\":" << (svc.active ? "true" : "false") << "}";
    }

    oss << "]}";

    return oss.str();
}

uint64_t MetricsAggregator::get_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

}  // namespace metrics
}  // namespace atsc3
