// test_metrics.cc — Unit tests for signal quality metrics

#include "metrics/ber_estimator.h"
#include "metrics/mer_estimator.h"
#include "metrics/metrics_aggregator.h"
#include "metrics/signal_strength.h"
#include "metrics/snr_estimator.h"

#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace metrics {
namespace {

// ============================================================================
// SNR Estimator Tests
// ============================================================================

class SnrEstimatorTest : public ::testing::Test {
protected:
    SnrEstimator estimator_;
};

TEST_F(SnrEstimatorTest, DefaultConstruction) {
    EXPECT_FALSE(estimator_.is_valid());
    EXPECT_DOUBLE_EQ(estimator_.get_snr_db(), 0.0);
}

TEST_F(SnrEstimatorTest, ProcessPilotsUpdatesStats) {
    std::vector<ATSC3_SAMPLE_T> received = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};
    std::vector<ATSC3_SAMPLE_T> reference = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};

    estimator_.process_pilots(received.data(), reference.data(), 3);

    auto stats = estimator_.get_stats();
    EXPECT_EQ(stats.symbols_processed, 1u);
    EXPECT_EQ(stats.pilots_used, 3u);
}

TEST_F(SnrEstimatorTest, PerfectPilotsHighSNR) {
    // Perfect reception - no noise
    std::vector<ATSC3_SAMPLE_T> pilots = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};

    // Process multiple symbols to fill window
    for (int i = 0; i < 64; ++i) {
        estimator_.process_pilots(pilots.data(), pilots.data(), 4);
    }

    EXPECT_TRUE(estimator_.is_valid());
    // SNR should be at maximum (clamped)
    EXPECT_GE(estimator_.get_snr_db(), 40.0);
}

TEST_F(SnrEstimatorTest, NoisyPilotsLowerSNR) {
    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0f, 0.1f);

    std::vector<ATSC3_SAMPLE_T> reference = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};

    for (int i = 0; i < 100; ++i) {
        std::vector<ATSC3_SAMPLE_T> received;
        for (const auto& ref : reference) {
            received.emplace_back(ref.real() + noise(rng), ref.imag() + noise(rng));
        }
        estimator_.process_pilots(received.data(), reference.data(), 3);
    }

    EXPECT_TRUE(estimator_.is_valid());
    // With 10% noise, SNR should be around 20 dB (power ratio 100)
    EXPECT_GT(estimator_.get_snr_db(), 15.0);
    EXPECT_LT(estimator_.get_snr_db(), 25.0);
}

TEST_F(SnrEstimatorTest, ResetClearsState) {
    std::vector<ATSC3_SAMPLE_T> pilots = {{1.0f, 0.0f}};
    estimator_.process_pilots(pilots.data(), pilots.data(), 1);

    estimator_.reset();

    EXPECT_FALSE(estimator_.is_valid());
    auto stats = estimator_.get_stats();
    EXPECT_EQ(stats.symbols_processed, 0u);
}

// ============================================================================
// MER Estimator Tests
// ============================================================================

class MerEstimatorTest : public ::testing::Test {
protected:
    MerEstimator estimator_;
};

TEST_F(MerEstimatorTest, DefaultConstruction) {
    EXPECT_FALSE(estimator_.is_valid());
    EXPECT_DOUBLE_EQ(estimator_.get_mer_db(), 0.0);
}

TEST_F(MerEstimatorTest, SetConstellation) {
    estimator_.set_constellation(ConstellationType::QAM64);
    // Should not crash
    EXPECT_FALSE(estimator_.is_valid());
}

TEST_F(MerEstimatorTest, QPSKPerfectSymbols) {
    estimator_.set_constellation(ConstellationType::QPSK);

    // Perfect QPSK symbols at normalized positions
    float scale = 1.0f / std::sqrt(2.0f);
    std::vector<ATSC3_SAMPLE_T> symbols = {{scale, scale},
                                           {-scale, scale},
                                           {-scale, -scale},
                                           {scale, -scale}};

    for (int i = 0; i < 1024; ++i) {
        estimator_.process_symbols(symbols.data(), symbols.size());
    }

    EXPECT_TRUE(estimator_.is_valid());
    EXPECT_GT(estimator_.get_mer_db(), 40.0);
    EXPECT_LT(estimator_.get_evm_percent(), 1.0);
}

TEST_F(MerEstimatorTest, NoisyQPSKLowerMER) {
    estimator_.set_constellation(ConstellationType::QPSK);

    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0f, 0.05f);

    float scale = 1.0f / std::sqrt(2.0f);
    std::vector<ATSC3_SAMPLE_T> ideal = {{scale, scale},
                                         {-scale, scale},
                                         {-scale, -scale},
                                         {scale, -scale}};

    for (int i = 0; i < 1024; ++i) {
        std::vector<ATSC3_SAMPLE_T> noisy;
        for (const auto& sym : ideal) {
            noisy.emplace_back(sym.real() + noise(rng), sym.imag() + noise(rng));
        }
        estimator_.process_symbols(noisy.data(), noisy.size());
    }

    EXPECT_TRUE(estimator_.is_valid());
    // Should have reasonable MER with 5% noise
    EXPECT_GT(estimator_.get_mer_db(), 20.0);
    EXPECT_LT(estimator_.get_mer_db(), 35.0);
}

TEST_F(MerEstimatorTest, QAM16Symbols) {
    estimator_.set_constellation(ConstellationType::QAM16);

    float scale = 1.0f / std::sqrt(10.0f);
    std::vector<ATSC3_SAMPLE_T> symbols = {{scale, scale},
                                           {3 * scale, scale},
                                           {scale, 3 * scale},
                                           {3 * scale, 3 * scale}};

    for (int i = 0; i < 1024; ++i) {
        estimator_.process_symbols(symbols.data(), symbols.size());
    }

    EXPECT_TRUE(estimator_.is_valid());
    EXPECT_GT(estimator_.get_mer_db(), 40.0);
}

// ============================================================================
// BER Estimator Tests
// ============================================================================

class BerEstimatorTest : public ::testing::Test {
protected:
    BerEstimator estimator_;
};

TEST_F(BerEstimatorTest, DefaultConstruction) {
    EXPECT_FALSE(estimator_.is_valid());
    EXPECT_DOUBLE_EQ(estimator_.get_ber(), 0.0);
}

TEST_F(BerEstimatorTest, AllConvergedLowBER) {
    // All codewords converge quickly
    for (int i = 0; i < 100; ++i) {
        estimator_.process_codeword(5, true, 50);
    }

    EXPECT_TRUE(estimator_.is_valid());
    EXPECT_LT(estimator_.get_ber(), 1e-4);
    EXPECT_DOUBLE_EQ(estimator_.get_fer(), 0.0);
}

TEST_F(BerEstimatorTest, SlowConvergenceHigherBER) {
    // Codewords converge but take many iterations
    for (int i = 0; i < 100; ++i) {
        estimator_.process_codeword(40, true, 50);
    }

    EXPECT_TRUE(estimator_.is_valid());
    EXPECT_GT(estimator_.get_ber(), 1e-4);
    EXPECT_DOUBLE_EQ(estimator_.get_fer(), 0.0);
}

TEST_F(BerEstimatorTest, NonConvergenceHighBER) {
    // Half of codewords fail
    for (int i = 0; i < 100; ++i) {
        estimator_.process_codeword(50, i % 2 == 0, 50);
    }

    EXPECT_TRUE(estimator_.is_valid());
    EXPECT_GT(estimator_.get_ber(), 1e-3);
    EXPECT_NEAR(estimator_.get_fer(), 0.5, 0.05);
}

TEST_F(BerEstimatorTest, StatsTracking) {
    estimator_.process_codeword(10, true, 50);
    estimator_.process_codeword(20, false, 50);

    auto stats = estimator_.get_stats();
    EXPECT_EQ(stats.codewords_processed, 2u);
    EXPECT_EQ(stats.codewords_converged, 1u);
    EXPECT_EQ(stats.codewords_failed, 1u);
}

// ============================================================================
// Signal Strength Tests
// ============================================================================

class SignalStrengthTest : public ::testing::Test {
protected:
    SignalStrength strength_;
};

TEST_F(SignalStrengthTest, DefaultConstruction) {
    EXPECT_FALSE(strength_.is_valid());
    EXPECT_DOUBLE_EQ(strength_.get_rssi_dbm(), -120.0);
}

TEST_F(SignalStrengthTest, UpdateRSSI) {
    strength_.update_rssi(-60.0);

    EXPECT_TRUE(strength_.is_valid());
    EXPECT_DOUBLE_EQ(strength_.get_rssi_dbm(), -60.0);
}

TEST_F(SignalStrengthTest, RSSIAveraging) {
    // Update with varying values
    for (int i = 0; i < 16; ++i) {
        strength_.update_rssi(-60.0 + i);
    }

    auto stats = strength_.get_stats();
    // Average of -60 to -45 should be around -52.5
    EXPECT_NEAR(stats.rssi_dbm_avg, -52.5, 0.5);
}

TEST_F(SignalStrengthTest, AGCCorrection) {
    strength_.update_rssi(-60.0);
    strength_.update_agc_gain(20.0);

    // Corrected RSSI = raw - gain = -60 - 20 = -80
    EXPECT_NEAR(strength_.get_corrected_rssi_dbm(), -80.0, 0.1);
}

TEST_F(SignalStrengthTest, PeakAndMinTracking) {
    strength_.update_rssi(-50.0);
    strength_.update_rssi(-70.0);
    strength_.update_rssi(-60.0);

    auto stats = strength_.get_stats();
    EXPECT_DOUBLE_EQ(stats.rssi_dbm_peak, -50.0);
    EXPECT_DOUBLE_EQ(stats.rssi_dbm_min, -70.0);
}

// ============================================================================
// Metrics Aggregator Tests
// ============================================================================

class MetricsAggregatorTest : public ::testing::Test {
protected:
    MetricsAggregator aggregator_;
};

TEST_F(MetricsAggregatorTest, DefaultConstruction) {
    auto snapshot = aggregator_.get_snapshot();
    EXPECT_FALSE(snapshot.signal_locked);
    EXPECT_FALSE(snapshot.frame_locked);
}

TEST_F(MetricsAggregatorTest, LockStatus) {
    aggregator_.set_signal_locked(true);
    aggregator_.set_frame_locked(true);

    auto snapshot = aggregator_.get_snapshot();
    EXPECT_TRUE(snapshot.signal_locked);
    EXPECT_TRUE(snapshot.frame_locked);
}

TEST_F(MetricsAggregatorTest, ServiceList) {
    std::vector<ServiceInfo> services;
    ServiceInfo svc1;
    svc1.service_id = 1;
    std::snprintf(svc1.name.data(), svc1.name.size(), "Service 1");
    svc1.active = true;
    services.push_back(svc1);

    aggregator_.update_services(services);

    auto snapshot = aggregator_.get_snapshot();
    ASSERT_EQ(snapshot.services.size(), 1u);
    EXPECT_EQ(snapshot.services[0].service_id, 1u);
    EXPECT_TRUE(snapshot.services[0].active);
}

TEST_F(MetricsAggregatorTest, ToJsonNotEmpty) {
    std::string json = aggregator_.to_json();
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("rssi_dbm"), std::string::npos);
    EXPECT_NE(json.find("snr_db"), std::string::npos);
    EXPECT_NE(json.find("mer_db"), std::string::npos);
}

TEST_F(MetricsAggregatorTest, ToJsonCompactOneLine) {
    std::string json = aggregator_.to_json_compact();
    EXPECT_FALSE(json.empty());
    // Compact JSON should not have newlines
    EXPECT_EQ(json.find('\n'), std::string::npos);
}

TEST_F(MetricsAggregatorTest, TimestampIncreases) {
    auto snapshot1 = aggregator_.get_snapshot();
    // Small delay
    for (volatile int i = 0; i < 1000000; ++i) {
    }
    auto snapshot2 = aggregator_.get_snapshot();

    EXPECT_GE(snapshot2.timestamp_ms, snapshot1.timestamp_ms);
    EXPECT_GE(snapshot2.uptime_ms, snapshot1.uptime_ms);
}

TEST_F(MetricsAggregatorTest, IntegratedMetrics) {
    // Update all metrics
    aggregator_.signal_strength().update_rssi(-55.0);
    aggregator_.signal_strength().update_agc_gain(10.0);

    std::vector<ATSC3_SAMPLE_T> pilots = {{1.0f, 0.0f}, {0.0f, 1.0f}};
    aggregator_.snr_estimator().process_pilots(pilots.data(), pilots.data(), 2);

    aggregator_.mer_estimator().set_constellation(ConstellationType::QPSK);
    float scale = 1.0f / std::sqrt(2.0f);
    std::vector<ATSC3_SAMPLE_T> symbols = {{scale, scale}};
    aggregator_.mer_estimator().process_symbols(symbols.data(), 1);

    aggregator_.ber_estimator().process_codeword(5, true, 50);

    aggregator_.set_signal_locked(true);
    aggregator_.set_frame_locked(true);

    auto snapshot = aggregator_.get_snapshot();
    EXPECT_NE(snapshot.rssi_dbm, -120.0);
    EXPECT_TRUE(snapshot.signal_locked);
    EXPECT_TRUE(snapshot.frame_locked);
}

}  // namespace
}  // namespace metrics
}  // namespace atsc3
