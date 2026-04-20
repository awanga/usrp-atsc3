// test_equalizer.cc — Unit tests for lib/channel/equalizer.h
//
// Tests ZF and MMSE equalization, phase tracking, and EVM computation

#include <gtest/gtest.h>

#include "equalizer.h"
#include "pilot_extractor.h"

#include <cmath>
#include <random>
#include <vector>

namespace atsc3 {
namespace channel {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(EqualizerTest, DefaultConstruction) {
    Equalizer equalizer;

    auto config = equalizer.get_config();
    EXPECT_EQ(config.fft_size, 8192u);
    EXPECT_EQ(config.mode, EqualizationMode::ZERO_FORCING);
    EXPECT_TRUE(config.enable_phase_tracking);
}

TEST(EqualizerTest, CustomConfig) {
    EqualizerConfig config;
    config.fft_size = 16384;
    config.mode = EqualizationMode::MMSE;
    config.noise_variance = 0.05f;
    config.enable_phase_tracking = false;

    Equalizer equalizer(config);

    EXPECT_EQ(equalizer.get_config().fft_size, 16384u);
    EXPECT_EQ(equalizer.get_config().mode, EqualizationMode::MMSE);
    EXPECT_FALSE(equalizer.get_config().enable_phase_tracking);
}

TEST(EqualizerTest, InvalidFftSizeThrows) {
    EqualizerConfig config;
    config.fft_size = 4096;  // Invalid

    EXPECT_THROW(Equalizer equalizer(config), std::invalid_argument);
}

TEST(EqualizerTest, ValidFftSizes) {
    std::vector<size_t> valid_sizes = {8192, 16384, 32768};

    for (size_t size : valid_sizes) {
        EqualizerConfig config;
        config.fft_size = size;

        EXPECT_NO_THROW({
            Equalizer equalizer(config);
            EXPECT_GT(equalizer.get_num_active_carriers(), 0u);
        }) << "Failed for FFT size " << size;
    }
}

TEST(EqualizerTest, ActiveCarrierCounts) {
    // 8K
    {
        EqualizerConfig config;
        config.fft_size = 8192;
        Equalizer equalizer(config);
        EXPECT_EQ(equalizer.get_num_active_carriers(), 6913u);
    }

    // 16K
    {
        EqualizerConfig config;
        config.fft_size = 16384;
        Equalizer equalizer(config);
        EXPECT_EQ(equalizer.get_num_active_carriers(), 13825u);
    }

    // 32K
    {
        EqualizerConfig config;
        config.fft_size = 32768;
        Equalizer equalizer(config);
        EXPECT_EQ(equalizer.get_num_active_carriers(), 27649u);
    }
}

//==============================================================================
// Zero-Forcing Equalization Tests
//==============================================================================

TEST(EqualizerTest, ZFUnityChannel) {
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

    // Unity channel: H = 1
    // Received = Transmitted (no distortion)
    size_t num_carriers = 100;
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        received[k] = sample_t(16384, 8192);  // 0.5 + 0.25j
        channel[k] = sample_t(32767, 0);       // 1 + 0j
#else
        received[k] = sample_t(0.5f, 0.25f);
        channel[k] = sample_t(1.0f, 0.0f);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.symbols.size(), num_carriers);

    // Equalized should match received for unity channel
    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(result.symbols[k].real()), 0.5f, 0.05f);
        EXPECT_NEAR(q15_to_float(result.symbols[k].imag()), 0.25f, 0.05f);
#else
        EXPECT_NEAR(result.symbols[k].real(), 0.5f, 1e-5f);
        EXPECT_NEAR(result.symbols[k].imag(), 0.25f, 1e-5f);
#endif
    }
}

TEST(EqualizerTest, ZFAttenuatedChannel) {
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

    // Channel H = 0.5 (attenuated)
    // Received = H * Transmitted = 0.5 * (1 + 0j)
    size_t num_carriers = 100;
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        received[k] = sample_t(16384, 0);   // 0.5 + 0j (received)
        channel[k] = sample_t(16384, 0);    // 0.5 + 0j (channel)
#else
        received[k] = sample_t(0.5f, 0.0f);
        channel[k] = sample_t(0.5f, 0.0f);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);

    // Equalized should be 1.0 (0.5 / 0.5)
    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(result.symbols[k].real()), 1.0f, 0.1f);
        EXPECT_NEAR(q15_to_float(result.symbols[k].imag()), 0.0f, 0.05f);
#else
        EXPECT_NEAR(result.symbols[k].real(), 1.0f, 1e-5f);
        EXPECT_NEAR(result.symbols[k].imag(), 0.0f, 1e-5f);
#endif
    }
}

TEST(EqualizerTest, ZFComplexChannel) {
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

    // Channel H = 0.6 + 0.8j (|H| = 1, phase = 53.13°)
    // Transmitted X = 1 + 0j
    // Received Y = H * X = 0.6 + 0.8j
    // Equalized = Y / H = 1 + 0j
    size_t num_carriers = 50;
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        received[k] = sample_t(float_to_q15(0.6f), float_to_q15(0.8f));
        channel[k] = sample_t(float_to_q15(0.6f), float_to_q15(0.8f));
#else
        received[k] = sample_t(0.6f, 0.8f);
        channel[k] = sample_t(0.6f, 0.8f);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(result.symbols[k].real()), 1.0f, 0.1f);
        EXPECT_NEAR(q15_to_float(result.symbols[k].imag()), 0.0f, 0.1f);
#else
        EXPECT_NEAR(result.symbols[k].real(), 1.0f, 1e-4f);
        EXPECT_NEAR(result.symbols[k].imag(), 0.0f, 1e-4f);
#endif
    }
}

TEST(EqualizerTest, ZFDeepFadeZeros) {
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    config.min_channel_magnitude = 0.1f;
    Equalizer equalizer(config);

    // Deep fade: H ≈ 0 at some carriers
    size_t num_carriers = 50;
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        received[k] = sample_t(16384, 0);
        // Deep fade at carrier 25
        if (k == 25) {
            channel[k] = sample_t(10, 0);  // Very small
        } else {
            channel[k] = sample_t(32767, 0);
        }
#else
        received[k] = sample_t(0.5f, 0.0f);
        if (k == 25) {
            channel[k] = sample_t(0.001f, 0.0f);  // Very small
        } else {
            channel[k] = sample_t(1.0f, 0.0f);
        }
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_GE(result.num_zeroed_carriers, 1u);

    // Deep fade carrier should be zeroed
#ifdef ATSC3_FIXED_POINT
    EXPECT_EQ(result.symbols[25].real(), 0);
    EXPECT_EQ(result.symbols[25].imag(), 0);
#else
    EXPECT_FLOAT_EQ(result.symbols[25].real(), 0.0f);
    EXPECT_FLOAT_EQ(result.symbols[25].imag(), 0.0f);
#endif
}

//==============================================================================
// MMSE Equalization Tests
//==============================================================================

TEST(EqualizerTest, MMSEUnityChannel) {
    EqualizerConfig config;
    config.mode = EqualizationMode::MMSE;
    config.noise_variance = 0.01f;
    Equalizer equalizer(config);

    size_t num_carriers = 100;
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        received[k] = sample_t(16384, 8192);
        channel[k] = sample_t(32767, 0);
#else
        received[k] = sample_t(0.5f, 0.25f);
        channel[k] = sample_t(1.0f, 0.0f);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);

    // MMSE should be close to ZF for high SNR (unity channel)
    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(result.symbols[k].real()), 0.5f, 0.1f);
        EXPECT_NEAR(q15_to_float(result.symbols[k].imag()), 0.25f, 0.1f);
#else
        EXPECT_NEAR(result.symbols[k].real(), 0.5f, 0.02f);
        EXPECT_NEAR(result.symbols[k].imag(), 0.25f, 0.02f);
#endif
    }
}

TEST(EqualizerTest, MMSERegularization) {
    // MMSE should regularize and produce smaller output for weak channels
    EqualizerConfig config_mmse;
    config_mmse.mode = EqualizationMode::MMSE;
    config_mmse.noise_variance = 0.1f;  // High noise
    config_mmse.min_channel_magnitude = 0.001f;  // Allow small channels
    Equalizer eq_mmse(config_mmse);

    EqualizerConfig config_zf;
    config_zf.mode = EqualizationMode::ZERO_FORCING;
    config_zf.min_channel_magnitude = 0.001f;
    Equalizer eq_zf(config_zf);

    // Weak channel
    std::vector<sample_t> received(1);
    std::vector<sample_t> channel(1);

#ifdef ATSC3_FIXED_POINT
    received[0] = sample_t(1638, 0);   // 0.05
    channel[0] = sample_t(3277, 0);    // 0.1
#else
    received[0] = sample_t(0.05f, 0.0f);
    channel[0] = sample_t(0.1f, 0.0f);
#endif

    auto result_mmse = eq_mmse.equalize(received.data(), channel, 0);
    auto result_zf = eq_zf.equalize(received.data(), channel, 0);

    // MMSE should have smaller magnitude than ZF due to regularization
#ifdef ATSC3_FIXED_POINT
    float mmse_mag = std::abs(q15_to_float(result_mmse.symbols[0].real()));
    float zf_mag = std::abs(q15_to_float(result_zf.symbols[0].real()));
#else
    float mmse_mag = std::abs(result_mmse.symbols[0].real());
    float zf_mag = std::abs(result_zf.symbols[0].real());
#endif

    EXPECT_LT(mmse_mag, zf_mag);
}

//==============================================================================
// Single Sample Equalization Tests
//==============================================================================

TEST(EqualizerTest, EqualizeSampleZF) {
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

#ifdef ATSC3_FIXED_POINT
    sample_t y(16384, 8192);  // 0.5 + 0.25j
    sample_t h(32767, 0);      // 1 + 0j
#else
    sample_t y(0.5f, 0.25f);
    sample_t h(1.0f, 0.0f);
#endif

    sample_t result = equalizer.equalize_sample(y, h);

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(result.real()), 0.5f, 0.05f);
    EXPECT_NEAR(q15_to_float(result.imag()), 0.25f, 0.05f);
#else
    EXPECT_NEAR(result.real(), 0.5f, 1e-5f);
    EXPECT_NEAR(result.imag(), 0.25f, 1e-5f);
#endif
}

TEST(EqualizerTest, EqualizeSampleMMSE) {
    EqualizerConfig config;
    config.mode = EqualizationMode::MMSE;
    config.noise_variance = 0.01f;
    Equalizer equalizer(config);

#ifdef ATSC3_FIXED_POINT
    sample_t y(16384, 8192);
    sample_t h(32767, 0);
#else
    sample_t y(0.5f, 0.25f);
    sample_t h(1.0f, 0.0f);
#endif

    sample_t result = equalizer.equalize_sample(y, h);

    // Should be close to ZF result for unity channel
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(result.real()), 0.5f, 0.1f);
    EXPECT_NEAR(q15_to_float(result.imag()), 0.25f, 0.1f);
#else
    EXPECT_NEAR(result.real(), 0.5f, 0.02f);
    EXPECT_NEAR(result.imag(), 0.25f, 0.02f);
#endif
}

//==============================================================================
// Null Input Tests
//==============================================================================

TEST(EqualizerTest, NullReceivedReturnsInvalid) {
    Equalizer equalizer;
    std::vector<sample_t> channel(100);

    auto result = equalizer.equalize(nullptr, channel, 0);

    EXPECT_FALSE(result.is_valid);
}

TEST(EqualizerTest, EmptyChannelReturnsInvalid) {
    Equalizer equalizer;
    std::vector<sample_t> received(100);

    auto result = equalizer.equalize(received.data(), {}, 0);

    EXPECT_FALSE(result.is_valid);
}

//==============================================================================
// EVM Computation Tests (Key Spec Requirement)
//==============================================================================

TEST(EqualizerTest, EVMPerfectEqualization) {
    // Test requirement: "equalized constellation EVM < 1% in AWGN"
    // 1% EVM = -40 dB

    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

    // Perfect equalization: equalized = reference
    std::vector<sample_t> equalized(1000);
    std::vector<sample_t> reference(1000);

    for (size_t k = 0; k < 1000; ++k) {
        // QPSK constellation points
        float re = ((k % 2) == 0) ? 0.707f : -0.707f;
        float im = ((k / 2) % 2 == 0) ? 0.707f : -0.707f;
#ifdef ATSC3_FIXED_POINT
        equalized[k] = sample_t(float_to_q15(re), float_to_q15(im));
        reference[k] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        equalized[k] = sample_t(re, im);
        reference[k] = sample_t(re, im);
#endif
    }

    float evm_db = compute_evm_db(equalized, reference);

    // Perfect equalization should have very low EVM
    EXPECT_LT(evm_db, -40.0f);
}

TEST(EqualizerTest, EVMWithNoise) {
    std::vector<sample_t> equalized(1000);
    std::vector<sample_t> reference(1000);

    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0f, 0.01f);  // ~40 dB SNR

    for (size_t k = 0; k < 1000; ++k) {
        float re = ((k % 2) == 0) ? 0.707f : -0.707f;
        float im = ((k / 2) % 2 == 0) ? 0.707f : -0.707f;

#ifdef ATSC3_FIXED_POINT
        reference[k] = sample_t(float_to_q15(re), float_to_q15(im));
        equalized[k] = sample_t(float_to_q15(re + noise(rng)),
                                 float_to_q15(im + noise(rng)));
#else
        reference[k] = sample_t(re, im);
        equalized[k] = sample_t(re + noise(rng), im + noise(rng));
#endif
    }

    float evm_db = compute_evm_db(equalized, reference);

    // With ~40 dB SNR noise, EVM should be around -40 dB
    // Allow some margin
    EXPECT_LT(evm_db, -30.0f);
    EXPECT_GT(evm_db, -60.0f);
}

TEST(EqualizerTest, EVMKnownChannelAWGN) {
    // Full test: known channel in AWGN → EVM < 1% (-40 dB)
    EqualizerConfig config;
    config.mode = EqualizationMode::ZERO_FORCING;
    Equalizer equalizer(config);

    std::mt19937 rng(123);
    std::normal_distribution<float> noise(0.0f, 0.005f);  // ~46 dB SNR

    size_t num_carriers = 1000;
    std::vector<sample_t> transmitted(num_carriers);
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    // Unity channel with AWGN
    for (size_t k = 0; k < num_carriers; ++k) {
        // QPSK symbol
        float tx_re = ((k % 2) == 0) ? 0.707f : -0.707f;
        float tx_im = ((k / 2) % 2 == 0) ? 0.707f : -0.707f;

        // Add noise to received
        float rx_re = tx_re + noise(rng);
        float rx_im = tx_im + noise(rng);

#ifdef ATSC3_FIXED_POINT
        transmitted[k] = sample_t(float_to_q15(tx_re), float_to_q15(tx_im));
        received[k] = sample_t(float_to_q15(rx_re), float_to_q15(rx_im));
        channel[k] = sample_t(32767, 0);  // Unity channel
#else
        transmitted[k] = sample_t(tx_re, tx_im);
        received[k] = sample_t(rx_re, rx_im);
        channel[k] = sample_t(1.0f, 0.0f);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);

    float evm_db = compute_evm_db(result.symbols, transmitted);

    // Requirement: EVM < 1% (-40 dB)
    EXPECT_LT(evm_db, -35.0f) << "EVM too high: " << evm_db << " dB";
}

//==============================================================================
// Phase Tracking Tests
//==============================================================================

TEST(EqualizerTest, PhaseTrackingDisabled) {
    EqualizerConfig config;
    config.enable_phase_tracking = false;
    Equalizer equalizer(config);

    EXPECT_FLOAT_EQ(equalizer.get_residual_phase(), 0.0f);
}

TEST(EqualizerTest, Reset) {
    Equalizer equalizer;

    // Simulate some processing
    equalizer.reset();

    EXPECT_FLOAT_EQ(equalizer.get_residual_phase(), 0.0f);
}

//==============================================================================
// Reconfiguration Tests
//==============================================================================

TEST(EqualizerTest, Reconfigure) {
    Equalizer equalizer;

    EXPECT_EQ(equalizer.get_config().fft_size, 8192u);
    EXPECT_EQ(equalizer.get_config().mode, EqualizationMode::ZERO_FORCING);

    EqualizerConfig new_config;
    new_config.fft_size = 16384;
    new_config.mode = EqualizationMode::MMSE;

    equalizer.set_config(new_config);

    EXPECT_EQ(equalizer.get_config().fft_size, 16384u);
    EXPECT_EQ(equalizer.get_config().mode, EqualizationMode::MMSE);
    EXPECT_EQ(equalizer.get_num_active_carriers(), 13825u);
}

//==============================================================================
// Integration Tests
//==============================================================================

TEST(EqualizerTest, FullEqualizationPipeline) {
    // Simulate full pipeline: TX → Channel → RX → Equalize

    EqualizerConfig config;
    config.mode = EqualizationMode::MMSE;
    config.noise_variance = 0.01f;
    Equalizer equalizer(config);

    std::mt19937 rng(456);
    std::normal_distribution<float> noise(0.0f, 0.01f);

    size_t num_carriers = 500;
    std::vector<sample_t> transmitted(num_carriers);
    std::vector<sample_t> received(num_carriers);
    std::vector<sample_t> channel(num_carriers);

    // Frequency-selective channel (multipath)
    for (size_t k = 0; k < num_carriers; ++k) {
        // Channel varies across frequency
        float h_mag = 0.5f + 0.5f * std::cos(2.0f * static_cast<float>(M_PI) *
                                              static_cast<float>(k) / 100.0f);
        float h_phase = 0.3f * std::sin(2.0f * static_cast<float>(M_PI) *
                                         static_cast<float>(k) / 50.0f);

        float h_re = h_mag * std::cos(h_phase);
        float h_im = h_mag * std::sin(h_phase);

        // QPSK symbols
        float tx_re = ((k % 2) == 0) ? 0.707f : -0.707f;
        float tx_im = ((k / 2) % 2 == 0) ? 0.707f : -0.707f;

        // Received = H * Transmitted + noise
        float rx_re = h_re * tx_re - h_im * tx_im + noise(rng);
        float rx_im = h_re * tx_im + h_im * tx_re + noise(rng);

#ifdef ATSC3_FIXED_POINT
        transmitted[k] = sample_t(float_to_q15(tx_re), float_to_q15(tx_im));
        received[k] = sample_t(float_to_q15(rx_re), float_to_q15(rx_im));
        channel[k] = sample_t(float_to_q15(h_re), float_to_q15(h_im));
#else
        transmitted[k] = sample_t(tx_re, tx_im);
        received[k] = sample_t(rx_re, rx_im);
        channel[k] = sample_t(h_re, h_im);
#endif
    }

    auto result = equalizer.equalize(received.data(), channel, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.symbols.size(), num_carriers);

    // Compute EVM
    float evm_db = compute_evm_db(result.symbols, transmitted);

    // EVM threshold depends on mode:
    // - Float mode: Should achieve < -20 dB with proper equalization
    // - Fixed-point: Quantization noise limits achievable EVM
#ifdef ATSC3_FIXED_POINT
    // Fixed-point has higher quantization noise
    EXPECT_LT(evm_db, -5.0f) << "EVM too high after equalization: " << evm_db << " dB";
#else
    EXPECT_LT(evm_db, -20.0f) << "EVM too high after equalization: " << evm_db << " dB";
#endif
}

}  // namespace
}  // namespace channel
}  // namespace atsc3
