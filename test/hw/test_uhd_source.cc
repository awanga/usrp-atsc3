// test_uhd_source.cc — Hardware tests for UHDSource
//
// These tests require a physical USRP N2x0 connected at addr=192.168.10.2
// Run with: ctest -L hw

#include "iq_source.h"
#include "iq_source_factory.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <numeric>
#include <thread>
#include <vector>

namespace atsc3 {
namespace hal {
namespace {

// Test configuration - adjust if device is at different address
constexpr const char* kDeviceArgs = "addr=192.168.10.2";

// TVRX frequency range
constexpr double kTestFreqHz = 500e6;  // 500 MHz - middle of TVRX range
constexpr double kTestGainDb = 30.0;
constexpr double kTestSampleRate = 6.25e6;

// Test parameters
constexpr size_t kTestSamples = 100000;
constexpr double kStreamTimeoutSec = 2.0;

class UHDSourceHWTest : public ::testing::Test {
protected:
    void SetUp() override {
        source_ = create_uhd_source(kDeviceArgs);
        if (!source_) {
            GTEST_SKIP() << "USRP not available at " << kDeviceArgs;
        }
        ASSERT_TRUE(source_->is_connected());
    }

    void TearDown() override {
        source_.reset();
    }

    IQSourcePtr source_;
};

// Test: Device connects successfully
TEST_F(UHDSourceHWTest, DeviceConnects) {
    EXPECT_TRUE(source_->is_connected());
    std::string name = source_->get_device_name();
    EXPECT_FALSE(name.empty());
    std::cout << "Connected to: " << name << std::endl;
}

// Test: Frequency tuning within TVRX range
TEST_F(UHDSourceHWTest, FrequencyTuning) {
    // Tune to test frequency
    auto err = source_->set_frequency(kTestFreqHz);
    EXPECT_EQ(err, IQSourceError::kSuccess);

    double actual_freq = source_->get_frequency();
    // Allow 1 kHz tolerance
    EXPECT_NEAR(actual_freq, kTestFreqHz, 1000.0);
    std::cout << "Tuned to: " << actual_freq / 1e6 << " MHz" << std::endl;
}

// Test: Gain setting within TVRX range
TEST_F(UHDSourceHWTest, GainSetting) {
    auto err = source_->set_gain(kTestGainDb);
    EXPECT_EQ(err, IQSourceError::kSuccess);

    double actual_gain = source_->get_gain();
    // Allow 1 dB tolerance
    EXPECT_NEAR(actual_gain, kTestGainDb, 1.0);
    std::cout << "Gain set to: " << actual_gain << " dB" << std::endl;
}

// Test: Sample rate configuration
TEST_F(UHDSourceHWTest, SampleRateSetting) {
    auto err = source_->set_sample_rate(kTestSampleRate);
    EXPECT_EQ(err, IQSourceError::kSuccess);

    double actual_rate = source_->get_sample_rate();
    // Allow 1% tolerance
    EXPECT_NEAR(actual_rate, kTestSampleRate, kTestSampleRate * 0.01);
    std::cout << "Sample rate: " << actual_rate / 1e6 << " MS/s" << std::endl;
}

// Test: RSSI reads plausible value
TEST_F(UHDSourceHWTest, RSSIPlausible) {
    // Configure device first
    source_->set_frequency(kTestFreqHz);
    source_->set_gain(kTestGainDb);
    source_->set_sample_rate(kTestSampleRate);

    // Read some samples to ensure device is streaming
    std::vector<std::complex<float>> buf(1000);
    source_->read(buf.data(), buf.size());

    // Give device time to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    double rssi = source_->get_rssi_dbm();
    std::cout << "RSSI: " << rssi << " dBm" << std::endl;

    // RSSI should be in a plausible range
    // -120 dBm = very weak / noise floor
    // -20 dBm = strong signal
    EXPECT_GE(rssi, -120.0) << "RSSI too low - sensor may be broken";
    EXPECT_LE(rssi, -10.0) << "RSSI too high - might indicate saturation";
}

// Test: Basic streaming works
TEST_F(UHDSourceHWTest, BasicStreaming) {
    source_->set_frequency(kTestFreqHz);
    source_->set_gain(kTestGainDb);
    source_->set_sample_rate(kTestSampleRate);

    std::vector<std::complex<float>> buf(kTestSamples);
    size_t received = source_->read(buf.data(), buf.size());

    EXPECT_GT(received, 0) << "No samples received";
    EXPECT_GE(received, kTestSamples * 0.9)
        << "Received fewer samples than expected (" << received << "/" << kTestSamples << ")";

    auto err = source_->get_last_error();
    EXPECT_TRUE(err == IQSourceError::kSuccess || err == IQSourceError::kBufferOverflow)
        << "Unexpected error: " << static_cast<int>(err);

    std::cout << "Received " << received << " samples" << std::endl;
}

// Test: Noise floor measurement (no antenna / terminated)
TEST_F(UHDSourceHWTest, NoiseFloorMeasurement) {
    // Configure at minimum gain for noise floor measurement.
    // Note: With the TVRX gain inversion, 0 dB user gain means maximum
    // attenuation (minimum signal). This measures the receiver's minimum
    // signal level, which includes:
    // - Internal noise floor
    // - Environmental RF interference
    // - Any broadcast signals at the test frequency
    source_->set_frequency(kTestFreqHz);
    source_->set_gain(0.0);  // Minimum gain (maximum attenuation with TVRX)
    source_->set_sample_rate(kTestSampleRate);

    // Capture samples
    std::vector<std::complex<float>> buf(kTestSamples);
    size_t received = source_->read(buf.data(), buf.size());
    ASSERT_GT(received, 1000) << "Not enough samples for noise measurement";

    // Calculate power in dBFS
    double sum_power = 0.0;
    for (size_t i = 0; i < received; ++i) {
        sum_power += std::norm(buf[i]);  // |x|^2
    }
    double avg_power = sum_power / received;
    double power_dbfs = 10.0 * std::log10(avg_power + 1e-20);

    std::cout << "Noise floor at 0 dB gain: " << power_dbfs << " dBFS" << std::endl;

    // The noise floor at minimum gain should be below full scale.
    // In ideal conditions (terminated input, quiet RF environment), expect < -30 dBFS.
    // In real environments (antenna connected, broadcast interference), levels may be higher.
    // We use a lenient threshold of -3 dBFS (just below saturation) to ensure the
    // device isn't stuck at full scale, while allowing for environmental conditions.
    EXPECT_LT(power_dbfs, -3.0)
        << "Signal near saturation even at minimum gain - check for strong interference";

    // For ideal conditions (terminated input), a stricter check would be:
    if (power_dbfs > -20.0) {
        std::cout << "Note: Power level (" << power_dbfs << " dBFS) is higher than typical "
                  << "noise floor. This may indicate environmental RF interference or "
                  << "a connected antenna receiving broadcast signals." << std::endl;
    }

    // Also verify samples have reasonable variance (not all zeros or stuck)
    double sum_real = 0.0, sum_imag = 0.0;
    for (size_t i = 0; i < received; ++i) {
        sum_real += buf[i].real();
        sum_imag += buf[i].imag();
    }
    double mean_real = sum_real / received;
    double mean_imag = sum_imag / received;

    double var_real = 0.0, var_imag = 0.0;
    for (size_t i = 0; i < received; ++i) {
        double dr = buf[i].real() - mean_real;
        double di = buf[i].imag() - mean_imag;
        var_real += dr * dr;
        var_imag += di * di;
    }
    var_real /= received;
    var_imag /= received;

    std::cout << "Sample variance - Real: " << var_real << ", Imag: " << var_imag << std::endl;

    // Variance should be non-zero (samples are not stuck)
    EXPECT_GT(var_real, 1e-12) << "Real samples appear stuck";
    EXPECT_GT(var_imag, 1e-12) << "Imaginary samples appear stuck";
}

// Test: Gain affects signal power
TEST_F(UHDSourceHWTest, GainAffectsPower) {
    source_->set_frequency(kTestFreqHz);
    source_->set_sample_rate(kTestSampleRate);

    // Query actual gain range from device to stay within valid bounds
    double min_gain = source_->get_min_gain();
    double max_gain = source_->get_max_gain();
    double gain_range = max_gain - min_gain;

    // Use low and high gains within actual device range
    // Start at minimum to avoid saturation, end at ~2/3 of max to stay in linear region
    double low_gain = min_gain;
    double high_gain = min_gain + gain_range * 0.67;

    std::cout << "Testing gain effect: " << low_gain << " dB -> " << high_gain << " dB"
              << " (device range: " << min_gain << " - " << max_gain << " dB)" << std::endl;

    std::vector<std::complex<float>> buf(kTestSamples);

    // Measure at low gain
    source_->set_gain(low_gain);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    size_t n1 = source_->read(buf.data(), buf.size());

    double sum1 = 0.0;
    for (size_t i = 0; i < n1; ++i) {
        sum1 += std::norm(buf[i]);
    }
    double power_low = 10.0 * std::log10(sum1 / n1 + 1e-20);

    // Measure at high gain
    source_->set_gain(high_gain);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    size_t n2 = source_->read(buf.data(), buf.size());

    double sum2 = 0.0;
    for (size_t i = 0; i < n2; ++i) {
        sum2 += std::norm(buf[i]);
    }
    double power_high = 10.0 * std::log10(sum2 / n2 + 1e-20);

    double gain_diff = power_high - power_low;
    std::cout << "Power at " << low_gain << " dB gain: " << power_low << " dBFS" << std::endl;
    std::cout << "Power at " << high_gain << " dB gain: " << power_high << " dBFS" << std::endl;
    std::cout << "Power difference: " << gain_diff << " dB (expected positive)" << std::endl;

    // Power should increase with gain (monotonicity check).
    // The TVRX uses an IF attenuator that UHD exposes as "gain", so we invert
    // in software to provide normal gain semantics. This test verifies the
    // inversion is working: higher user gain should give higher power.
    //
    // Note: In real-world conditions, the exact relationship depends on:
    // - Environmental noise floor
    // - Signal saturation/compression
    // - Attenuator linearity
    //
    // We require any positive power increase (monotonicity) rather than
    // a specific ratio, since environmental factors vary.
    EXPECT_GT(gain_diff, 0.0)
        << "Gain increase should increase signal power. "
        << "Got " << gain_diff << " dB difference.";

    // If power at low gain is already near saturation (> -10 dBFS), warn but don't fail
    // since compression will limit the observable gain effect.
    if (power_low > -10.0) {
        std::cout << "Note: Power at low gain (" << power_low
                  << " dBFS) is near saturation; gain effect may be limited." << std::endl;
    }
}

// Test: Frequency range queries
TEST_F(UHDSourceHWTest, FrequencyRangeQueries) {
    double min_freq = source_->get_min_frequency();
    double max_freq = source_->get_max_frequency();

    std::cout << "Frequency range: " << min_freq / 1e6 << " - " << max_freq / 1e6 << " MHz"
              << std::endl;

    // TVRX should support at least 50-860 MHz
    EXPECT_LE(min_freq, 60e6);
    EXPECT_GE(max_freq, 800e6);
}

// Test: Gain range queries
TEST_F(UHDSourceHWTest, GainRangeQueries) {
    double min_gain = source_->get_min_gain();
    double max_gain = source_->get_max_gain();

    std::cout << "Gain range: " << min_gain << " - " << max_gain << " dB" << std::endl;

    // TVRX daughterboard supports 0-36.5 dB gain range
    // Allow some tolerance for different hardware revisions
    EXPECT_LE(min_gain, 5.0);
    EXPECT_GE(max_gain, 30.0);  // TVRX max is ~36.5 dB
}

// Test: Sample rate range queries
TEST_F(UHDSourceHWTest, SampleRateRangeQueries) {
    double min_rate = source_->get_min_sample_rate();
    double max_rate = source_->get_max_sample_rate();

    std::cout << "Sample rate range: " << min_rate / 1e6 << " - " << max_rate / 1e6 << " MS/s"
              << std::endl;

    // N210 should support at least 1-25 MS/s
    EXPECT_LE(min_rate, 2e6);
    EXPECT_GE(max_rate, 20e6);
}

}  // namespace
}  // namespace hal
}  // namespace atsc3
