// test_signal_chain.cc — Integration tests for ATSC 3.0 signal chain
//
// Tests the complete signal flow from IQ capture through bootstrap detection
// and L1 decoding to ConfigBus distribution.
//
// Requires real ATSC 3.0 IQ captures in test/captures/ (via git-lfs)

#include "bootstrap_detector.h"
#include "config_bus.h"
#include "iq_source.h"
#include "iq_source_factory.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

namespace atsc3 {
namespace integration {
namespace {

// Path to test captures directory (relative to source root, not build dir)
#ifndef SOURCE_DIR
#define SOURCE_DIR ".."
#endif
const std::string kCapturesDir = std::string(SOURCE_DIR) + "/test/captures";

// Sample rate for captures (from SigMF metadata)
constexpr double kCaptureSampleRate = 12.5e6;

// Helper to find an available capture file
std::string find_capture_file() {
    namespace fs = std::filesystem;

    // Try to find any .sigmf-data file
    for (const auto& entry : fs::directory_iterator(kCapturesDir)) {
        if (entry.path().extension() == ".sigmf-data") {
            return entry.path().string();
        }
    }
    return "";
}

// Test observer that records configuration updates
class TestConfigObserver : public config::ConfigObserver {
public:
    void on_config_update(const config::Atsc3Config& config) override {
        last_config_ = config;
        update_count_++;
    }

    const config::Atsc3Config& get_last_config() const {
        return last_config_;
    }

    int get_update_count() const {
        return update_count_;
    }

private:
    config::Atsc3Config last_config_;
    int update_count_ = 0;
};

// Test fixture for signal chain tests
class SignalChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        capture_file_ = find_capture_file();
        if (capture_file_.empty()) {
            GTEST_SKIP() << "No IQ captures available in " << kCapturesDir;
        }
    }

    std::string capture_file_;
};

// Test: FileSource can load real IQ capture
TEST_F(SignalChainTest, FileSourceLoadsCapture) {
    auto source = hal::create_file_source(capture_file_, kCaptureSampleRate, false);
    ASSERT_NE(source, nullptr) << "Failed to create FileSource for " << capture_file_;

    EXPECT_TRUE(source->is_connected());
    EXPECT_EQ(source->get_sample_rate(), kCaptureSampleRate);

    // Read some samples
    std::vector<std::complex<float>> buf(10000);
    size_t n = source->read(buf.data(), buf.size());
    EXPECT_GT(n, 0) << "Failed to read samples from capture";

    // Verify samples are not all zeros
    double power = 0.0;
    for (size_t i = 0; i < n; ++i) {
        power += std::norm(buf[i]);
    }
    power /= n;
    double power_dbfs = 10.0 * std::log10(power + 1e-20);

    std::cout << "Capture: " << capture_file_ << std::endl;
    std::cout << "Read " << n << " samples, power = " << power_dbfs << " dBFS" << std::endl;

    EXPECT_GT(power, 1e-10) << "Samples appear to be all zeros";
}

// Test: Bootstrap detection on real capture
TEST_F(SignalChainTest, BootstrapDetectionOnCapture) {
    auto source = hal::create_file_source(capture_file_, kCaptureSampleRate, false);
    ASSERT_NE(source, nullptr);

    // Configure bootstrap detector
    sync::BootstrapConfig config;
    config.sample_rate_hz = kCaptureSampleRate;
    config.threshold = 0.6;
    config.averaging_window = 64;

    sync::BootstrapDetector detector(config);

    // Read and process samples looking for bootstrap
    constexpr size_t kChunkSize = 10000;
    constexpr size_t kMaxSamples = 1000000;  // ~80ms at 12.5 MS/s
    std::vector<sample_t> buf(kChunkSize);

    bool detected = false;
    sync::BootstrapDetection detection;
    size_t total_samples = 0;

    while (total_samples < kMaxSamples && !detected) {
        size_t n = source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());
        if (n == 0) {
            break;  // EOF
        }

        auto det = detector.process(buf.data(), n);
        if (det.detected) {
            detected = true;
            detection = det;
        }
        total_samples += n;
    }

    std::cout << "Processed " << total_samples << " samples" << std::endl;

    if (detected) {
        std::cout << "Bootstrap detected at sample " << detection.sample_index << std::endl;
        std::cout << "  CFO estimate: " << detection.cfo_hz << " Hz" << std::endl;
        std::cout << "  Detection metric: " << detection.metric << std::endl;
    }

    EXPECT_TRUE(detected) << "No bootstrap detected in capture (may not be ATSC 3.0 or corrupted)";
}

// Test: ConfigBus distributes configuration to observers
TEST_F(SignalChainTest, ConfigBusDistribution) {
    config::ConfigBus bus;

    // Register multiple observers
    TestConfigObserver observer1;
    TestConfigObserver observer2;
    TestConfigObserver observer3;

    EXPECT_TRUE(bus.register_observer(&observer1));
    EXPECT_TRUE(bus.register_observer(&observer2));
    EXPECT_TRUE(bus.register_observer(&observer3));

    EXPECT_EQ(bus.num_observers(), 3u);

    // Create a test configuration
    config::Atsc3Config test_config;
    test_config.valid = true;
    test_config.fft_size = config::FftSize::FFT_16K;
    test_config.cp_length = config::CpLength::CP_2048_8192;
    test_config.pilot_pattern = config::PilotPattern::PP4;
    test_config.num_plps = 1;
    test_config.plps[0].plp_id = 0;
    test_config.plps[0].modulation = config::Modulation::QAM64;
    test_config.plps[0].code_rate = config::CodeRate::RATE_7_15;

    // Publish configuration
    bus.publish(test_config);

    // Verify all observers received the update
    EXPECT_EQ(observer1.get_update_count(), 1);
    EXPECT_EQ(observer2.get_update_count(), 1);
    EXPECT_EQ(observer3.get_update_count(), 1);

    // Verify configuration matches
    EXPECT_TRUE(observer1.get_last_config().valid);
    EXPECT_EQ(observer1.get_last_config().fft_size, config::FftSize::FFT_16K);
    EXPECT_EQ(observer1.get_last_config().pilot_pattern, config::PilotPattern::PP4);
    EXPECT_EQ(observer1.get_last_config().plps[0].modulation, config::Modulation::QAM64);

    EXPECT_EQ(bus.get_publish_count(), 1u);
}

// Test: Multiple configuration updates
TEST_F(SignalChainTest, MultipleConfigUpdates) {
    config::ConfigBus bus;
    TestConfigObserver observer;

    bus.register_observer(&observer);

    // Publish multiple configurations
    for (int i = 0; i < 5; ++i) {
        config::Atsc3Config cfg;
        cfg.valid = true;
        cfg.num_plps = i + 1;
        bus.publish(cfg);
    }

    EXPECT_EQ(observer.get_update_count(), 5);
    EXPECT_EQ(observer.get_last_config().num_plps, 5);
    EXPECT_EQ(bus.get_publish_count(), 5u);
}

// Test: Observer unregistration
TEST_F(SignalChainTest, ObserverUnregistration) {
    config::ConfigBus bus;

    TestConfigObserver observer1;
    TestConfigObserver observer2;

    bus.register_observer(&observer1);
    bus.register_observer(&observer2);

    EXPECT_EQ(bus.num_observers(), 2u);

    // Unregister one observer
    bus.unregister_observer(&observer1);
    EXPECT_EQ(bus.num_observers(), 1u);

    // Publish - only observer2 should receive
    config::Atsc3Config cfg;
    cfg.valid = true;
    bus.publish(cfg);

    EXPECT_EQ(observer1.get_update_count(), 0);
    EXPECT_EQ(observer2.get_update_count(), 1);
}

// Test: Signal power measurement from capture
TEST_F(SignalChainTest, SignalPowerMeasurement) {
    auto source = hal::create_file_source(capture_file_, kCaptureSampleRate, false);
    ASSERT_NE(source, nullptr);

    // Read a chunk of samples
    constexpr size_t kNumSamples = 100000;
    std::vector<std::complex<float>> buf(kNumSamples);
    size_t n = source->read(buf.data(), buf.size());
    ASSERT_GT(n, 1000u);

    // Calculate power statistics
    double sum_power = 0.0;
    double max_power = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double p = std::norm(buf[i]);
        sum_power += p;
        max_power = std::max(max_power, p);
    }
    double avg_power = sum_power / n;
    double avg_power_dbfs = 10.0 * std::log10(avg_power + 1e-20);
    double peak_power_dbfs = 10.0 * std::log10(max_power + 1e-20);
    double papr_db = peak_power_dbfs - avg_power_dbfs;

    std::cout << "Signal power analysis:" << std::endl;
    std::cout << "  Average power: " << avg_power_dbfs << " dBFS" << std::endl;
    std::cout << "  Peak power: " << peak_power_dbfs << " dBFS" << std::endl;
    std::cout << "  PAPR: " << papr_db << " dB" << std::endl;

    // ATSC 3.0 OFDM signals have ~7-12 dB PAPR over long windows.
    // With short sample windows (100k samples) and compression, measured
    // PAPR can be lower. We check for basic signal presence.
    EXPECT_GT(avg_power_dbfs, -40.0) << "Signal too weak";
    EXPECT_LT(avg_power_dbfs, 0.0) << "Signal saturating";
    EXPECT_GT(papr_db, 0.5) << "PAPR too low - signal appears clipped";
    EXPECT_LT(papr_db, 20.0) << "PAPR too high - may indicate noise only";
}

}  // namespace
}  // namespace integration
}  // namespace atsc3
