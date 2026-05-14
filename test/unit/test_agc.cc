// test_agc.cc — Unit tests for AGC Controller

#include <gtest/gtest.h>

#include "agc.h"

#include <cmath>
#include <complex>
#include <vector>

namespace atsc3 {
namespace hal {
namespace {

// Generate a buffer of samples with specified amplitude
std::vector<std::complex<float>> generate_samples(size_t n, float amplitude) {
    std::vector<std::complex<float>> samples(n);
    for (size_t i = 0; i < n; ++i) {
        // Simple sinusoid
        float phase = static_cast<float>(i) * 0.1f;
        samples[i] = std::complex<float>(amplitude * std::cos(phase),
                                         amplitude * std::sin(phase));
    }
    return samples;
}

TEST(AgcTest, DefaultConstruction) {
    AgcController agc;

    // Check default config values
    EXPECT_DOUBLE_EQ(agc.get_config().target_power_dbfs, -20.0);
    EXPECT_DOUBLE_EQ(agc.get_config().initial_gain_db, 30.0);
    EXPECT_DOUBLE_EQ(agc.get_gain(), 30.0);
}

TEST(AgcTest, CustomConfig) {
    AgcConfig config;
    config.target_power_dbfs = -10.0;
    config.initial_gain_db = 25.0;
    config.attack_samples = 500.0;
    config.release_samples = 5000.0;

    AgcController agc(config);

    EXPECT_DOUBLE_EQ(agc.get_config().target_power_dbfs, -10.0);
    EXPECT_DOUBLE_EQ(agc.get_gain(), 25.0);
}

TEST(AgcTest, ProcessEmptyBuffer) {
    AgcController agc;
    double initial_gain = agc.get_gain();

    // Empty buffer should not change gain
    double new_gain = agc.process(nullptr, 0);
    EXPECT_DOUBLE_EQ(new_gain, initial_gain);
}

TEST(AgcTest, StrongSignalReducesGain) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 30.0;
    config.attack_samples = 100.0;  // Fast attack for testing
    config.deadband_db = 0.5;

    AgcController agc(config);
    double initial_gain = agc.get_gain();

    // Generate strong signal (amplitude 1.0 = 0 dBFS, way above -20 dBFS target)
    auto samples = generate_samples(1000, 1.0f);

    // Process multiple times to allow AGC to converge
    double gain = initial_gain;
    for (int i = 0; i < 100; ++i) {
        gain = agc.process(samples.data(), samples.size());
    }

    // Gain should have decreased significantly
    EXPECT_LT(gain, initial_gain - 10.0);
}

TEST(AgcTest, WeakSignalIncreasesGain) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 10.0;  // Start low to allow room for increase
    config.release_samples = 100.0;  // Fast release for testing
    config.deadband_db = 0.5;

    AgcController agc(config);
    double initial_gain = agc.get_gain();

    // Generate weak signal (amplitude 0.01 = -40 dBFS, way below -20 dBFS target)
    auto samples = generate_samples(1000, 0.01f);

    // Process multiple times to allow AGC to converge
    double gain = initial_gain;
    for (int i = 0; i < 100; ++i) {
        gain = agc.process(samples.data(), samples.size());
    }

    // Gain should have increased significantly
    EXPECT_GT(gain, initial_gain + 10.0);
}

TEST(AgcTest, ConvergesToTarget) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 30.0;
    config.attack_samples = 50.0;
    config.release_samples = 50.0;
    config.deadband_db = 1.0;
    config.min_gain_db = 0.0;
    config.max_gain_db = 36.5;

    AgcController agc(config);

    // Generate signal at exactly the target level
    // -20 dBFS = amplitude sqrt(10^(-20/10)) = sqrt(0.01) = 0.1
    float target_amplitude = std::sqrt(std::pow(10.0f, -20.0f / 10.0f));
    auto samples = generate_samples(1000, target_amplitude);

    // Process many times
    for (int i = 0; i < 100; ++i) {
        agc.process(samples.data(), samples.size());
    }

    // Power should be at target (within deadband)
    double power = agc.get_power_dbfs();
    EXPECT_NEAR(power, config.target_power_dbfs, config.deadband_db + 0.5);
}

TEST(AgcTest, GainClampedToMin) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 10.0;  // Start near minimum
    config.min_gain_db = 0.0;
    config.attack_samples = 10.0;  // Very fast
    config.deadband_db = 0.1;

    AgcController agc(config);

    // Very strong signal should drive gain to minimum
    auto samples = generate_samples(1000, 10.0f);

    for (int i = 0; i < 1000; ++i) {
        agc.process(samples.data(), samples.size());
    }

    EXPECT_GE(agc.get_gain(), config.min_gain_db);
}

TEST(AgcTest, GainClampedToMax) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 35.0;  // Start near maximum
    config.max_gain_db = 36.5;
    config.release_samples = 10.0;  // Very fast
    config.deadband_db = 0.1;

    AgcController agc(config);

    // Very weak signal should drive gain to maximum
    auto samples = generate_samples(1000, 0.0001f);

    for (int i = 0; i < 1000; ++i) {
        agc.process(samples.data(), samples.size());
    }

    EXPECT_LE(agc.get_gain(), config.max_gain_db);
}

TEST(AgcTest, DeadbandPreventsHunting) {
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 30.0;
    config.attack_samples = 10.0;
    config.release_samples = 10.0;
    config.deadband_db = 3.0;  // Wide deadband

    AgcController agc(config);

    // Generate signal near target (within deadband)
    // -20 dBFS = amplitude 0.1, -18 dBFS = amplitude ~0.126 (within 3 dB)
    float amplitude = std::sqrt(std::pow(10.0f, -18.0f / 10.0f));
    auto samples = generate_samples(1000, amplitude);

    double initial_gain = agc.get_gain();

    // Process many times - gain should stay stable
    for (int i = 0; i < 100; ++i) {
        agc.process(samples.data(), samples.size());
    }

    // Gain should not have changed (within deadband)
    EXPECT_DOUBLE_EQ(agc.get_gain(), initial_gain);
}

TEST(AgcTest, Reset) {
    AgcConfig config;
    config.initial_gain_db = 30.0;

    AgcController agc(config);

    // Modify gain
    auto samples = generate_samples(1000, 1.0f);
    for (int i = 0; i < 100; ++i) {
        agc.process(samples.data(), samples.size());
    }

    EXPECT_NE(agc.get_gain(), config.initial_gain_db);

    // Reset
    agc.reset();

    EXPECT_DOUBLE_EQ(agc.get_gain(), config.initial_gain_db);
}

TEST(AgcTest, StepInputConvergence) {
    // Test that step input drives gain to within 1 dB of target within 100 iterations
    // (per TASKS.md requirement)
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 0.0;  // Start at minimum
    config.attack_samples = 100.0;
    config.release_samples = 100.0;
    config.deadband_db = 0.5;
    config.min_gain_db = 0.0;
    config.max_gain_db = 36.5;

    AgcController agc(config);

    // Generate signal that requires gain adjustment
    // At 0 dBFS input and 0 dB gain, we need to reach -20 dBFS output
    // This would require reducing the input power, which AGC can't do
    // Instead, test with weak input that needs gain increase

    // Signal at -40 dBFS (amplitude ~0.01)
    // To reach -20 dBFS, we need 20 dB of gain
    float amplitude = std::sqrt(std::pow(10.0f, -40.0f / 10.0f));
    auto samples = generate_samples(1000, amplitude);

    // Process 100 times
    for (int i = 0; i < 100; ++i) {
        agc.process(samples.data(), samples.size());
    }

    // The gain change should bring us close to target
    // Expected gain: ~20 dB to bring -40 dBFS to -20 dBFS
    // Allow 1 dB tolerance as specified in TASKS.md
    double expected_gain = 20.0;
    EXPECT_NEAR(agc.get_gain(), expected_gain, 1.0);
}

TEST(AgcTest, AsymmetricAttackRelease) {
    // Test that attack is faster than release
    AgcConfig config;
    config.target_power_dbfs = -20.0;
    config.initial_gain_db = 25.0;
    config.attack_samples = 100.0;    // Fast attack
    config.release_samples = 1000.0;  // Slow release
    config.deadband_db = 0.5;

    AgcController agc(config);

    // Strong signal (attack)
    auto strong_samples = generate_samples(1000, 1.0f);
    double gain_before_attack = agc.get_gain();

    agc.process(strong_samples.data(), strong_samples.size());
    double gain_after_one_attack = agc.get_gain();
    double attack_delta = gain_before_attack - gain_after_one_attack;

    // Reset and test release
    agc.reset();

    // Weak signal (release)
    auto weak_samples = generate_samples(1000, 0.001f);

    agc.process(weak_samples.data(), weak_samples.size());
    double gain_after_one_release = agc.get_gain();
    double release_delta = gain_after_one_release - config.initial_gain_db;

    // Attack should cause larger change than release
    // (in absolute terms, since attack reduces and release increases)
    EXPECT_GT(std::abs(attack_delta), std::abs(release_delta));
}

}  // namespace
}  // namespace hal
}  // namespace atsc3
