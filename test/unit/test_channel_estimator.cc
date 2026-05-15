// test_channel_estimator.cc — Unit tests for lib/channel/channel_estimator.h
//
// Tests LS channel estimation, interpolation, and SNR estimation

#include "channel_estimator.h"
#include "pilot_extractor.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace channel {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(ChannelEstimatorTest, DefaultConstruction) {
    ChannelEstimator estimator;

    auto config = estimator.get_config();
    EXPECT_EQ(config.fft_size, 8192u);
    EXPECT_EQ(config.backend, EstimatorBackend::LS_ONLY);
    EXPECT_EQ(config.interpolation, InterpolationMethod::LINEAR);
}

TEST(ChannelEstimatorTest, CustomConfig) {
    ChannelEstimatorConfig config;
    config.fft_size = 16384;
    config.backend = EstimatorBackend::LS_WIENER;
    config.enable_averaging = true;
    config.averaging_alpha = 0.2f;

    ChannelEstimator estimator(config);

    EXPECT_EQ(estimator.get_config().fft_size, 16384u);
    EXPECT_EQ(estimator.get_backend(), EstimatorBackend::LS_WIENER);
    EXPECT_TRUE(estimator.get_config().enable_averaging);
}

TEST(ChannelEstimatorTest, InvalidFftSizeThrows) {
    ChannelEstimatorConfig config;
    config.fft_size = 4096;  // Invalid

    EXPECT_THROW(ChannelEstimator estimator(config), std::invalid_argument);
}

TEST(ChannelEstimatorTest, ValidFftSizes) {
    std::vector<size_t> valid_sizes = {8192, 16384, 32768};

    for (size_t size : valid_sizes) {
        ChannelEstimatorConfig config;
        config.fft_size = size;

        EXPECT_NO_THROW({
            ChannelEstimator estimator(config);
            EXPECT_GT(estimator.get_num_active_carriers(), 0u);
        }) << "Failed for FFT size "
           << size;
    }
}

TEST(ChannelEstimatorTest, ActiveCarrierCounts) {
    // 8K
    {
        ChannelEstimatorConfig config;
        config.fft_size = 8192;
        ChannelEstimator estimator(config);
        EXPECT_EQ(estimator.get_num_active_carriers(), 6913u);
    }

    // 16K
    {
        ChannelEstimatorConfig config;
        config.fft_size = 16384;
        ChannelEstimator estimator(config);
        EXPECT_EQ(estimator.get_num_active_carriers(), 13825u);
    }

    // 32K
    {
        ChannelEstimatorConfig config;
        config.fft_size = 32768;
        ChannelEstimator estimator(config);
        EXPECT_EQ(estimator.get_num_active_carriers(), 27649u);
    }
}

//==============================================================================
// Backend Tests
//==============================================================================

TEST(ChannelEstimatorTest, SetBackend) {
    ChannelEstimator estimator;

    estimator.set_backend(EstimatorBackend::LS_WIENER);
    EXPECT_EQ(estimator.get_backend(), EstimatorBackend::LS_WIENER);

    estimator.set_backend(EstimatorBackend::LS_ONLY);
    EXPECT_EQ(estimator.get_backend(), EstimatorBackend::LS_ONLY);
}

TEST(ChannelEstimatorTest, MLBackendNotImplemented) {
    ChannelEstimator estimator;

    EXPECT_THROW(estimator.set_backend(EstimatorBackend::ML_ONNX), std::runtime_error);
}

//==============================================================================
// LS Estimation Tests
//==============================================================================

TEST(ChannelEstimatorTest, EstimateEmptyPilots) {
    ChannelEstimator estimator;

    std::vector<ofdm::PilotSymbol> empty_pilots;
    auto result = estimator.estimate(empty_pilots, 0);

    EXPECT_FALSE(result.is_valid);
}

TEST(ChannelEstimatorTest, EstimateUnityChannel) {
    ChannelEstimator estimator;

    // Create pilot with unity channel (received = reference)
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = 1000;

#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);  // +1
    pilot.received_value = sample_t(32767, 0);   // +1
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto result = estimator.estimate(pilots, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_GT(result.h_estimate.size(), 0u);
}

TEST(ChannelEstimatorTest, EstimateAtPilotsUnityChannel) {
    ChannelEstimator estimator;

    // Create pilots with unity channel
    std::vector<ofdm::PilotSymbol> pilots;
    for (size_t k = 0; k < 10; ++k) {
        ofdm::PilotSymbol pilot;
        pilot.subcarrier_index = 700 + k * 100;

#ifdef ATSC3_FIXED_POINT
        pilot.reference_value = sample_t(32767, 0);
        pilot.received_value = sample_t(32767, 0);
#else
        pilot.reference_value = sample_t(1.0f, 0.0f);
        pilot.received_value = sample_t(1.0f, 0.0f);
#endif
        pilot.type = ofdm::PilotType::SCATTERED;
        pilots.push_back(pilot);
    }

    auto h_pilots = estimator.estimate_at_pilots(pilots);

    EXPECT_EQ(h_pilots.size(), pilots.size());

    // All estimates should be ~1+j0
    for (const auto& h : h_pilots) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(h.real()), 1.0f, 0.05f);
        EXPECT_NEAR(q15_to_float(h.imag()), 0.0f, 0.05f);
#else
        EXPECT_NEAR(h.real(), 1.0f, 1e-5f);
        EXPECT_NEAR(h.imag(), 0.0f, 1e-5f);
#endif
    }
}

TEST(ChannelEstimatorTest, EstimateWithPhaseRotation) {
    ChannelEstimator estimator;

    // Channel has 90 degree phase rotation: H = j
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = 1000;

#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);  // +1
    pilot.received_value = sample_t(0, 32767);   // +j (received = H * ref = j * 1)
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto h_pilots = estimator.estimate_at_pilots(pilots);

    ASSERT_EQ(h_pilots.size(), 1u);

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h_pilots[0].real()), 0.0f, 0.05f);
    EXPECT_NEAR(q15_to_float(h_pilots[0].imag()), 1.0f, 0.05f);
#else
    EXPECT_NEAR(h_pilots[0].real(), 0.0f, 1e-5f);
    EXPECT_NEAR(h_pilots[0].imag(), 1.0f, 1e-5f);
#endif
}

TEST(ChannelEstimatorTest, EstimateWithAttenuation) {
    ChannelEstimator estimator;

    // Channel has 0.5 gain: H = 0.5
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = 1000;

#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);  // +1
    pilot.received_value = sample_t(16384, 0);   // +0.5
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.5f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto h_pilots = estimator.estimate_at_pilots(pilots);

    ASSERT_EQ(h_pilots.size(), 1u);

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h_pilots[0].real()), 0.5f, 0.05f);
    EXPECT_NEAR(q15_to_float(h_pilots[0].imag()), 0.0f, 0.05f);
#else
    EXPECT_NEAR(h_pilots[0].real(), 0.5f, 1e-5f);
    EXPECT_NEAR(h_pilots[0].imag(), 0.0f, 1e-5f);
#endif
}

TEST(ChannelEstimatorTest, EstimateWithNegativeReference) {
    ChannelEstimator estimator;

    // Reference is -1 (BPSK), received is -0.8 (attenuated unity channel)
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = 1000;

#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(-32767, 0);  // -1
    pilot.received_value = sample_t(-26214, 0);   // -0.8 (~-0.8 * 32768)
#else
    pilot.reference_value = sample_t(-1.0f, 0.0f);
    pilot.received_value = sample_t(-0.8f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto h_pilots = estimator.estimate_at_pilots(pilots);

    ASSERT_EQ(h_pilots.size(), 1u);

    // H = received / reference = -0.8 / -1 = 0.8
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h_pilots[0].real()), 0.8f, 0.1f);
    EXPECT_NEAR(q15_to_float(h_pilots[0].imag()), 0.0f, 0.05f);
#else
    EXPECT_NEAR(h_pilots[0].real(), 0.8f, 1e-5f);
    EXPECT_NEAR(h_pilots[0].imag(), 0.0f, 1e-5f);
#endif
}

//==============================================================================
// Interpolation Tests
//==============================================================================

TEST(ChannelEstimatorTest, InterpolationLinearBetweenPilots) {
    ChannelEstimatorConfig config;
    config.fft_size = 8192;
    config.interpolation = InterpolationMethod::LINEAR;
    ChannelEstimator estimator(config);

    size_t first_active = estimator.get_first_active_carrier();

    // Create two pilots with different channel values
    std::vector<ofdm::PilotSymbol> pilots;

    // First pilot at relative position 100, H = 1.0
    ofdm::PilotSymbol pilot1;
    pilot1.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot1.reference_value = sample_t(32767, 0);
    pilot1.received_value = sample_t(32767, 0);
#else
    pilot1.reference_value = sample_t(1.0f, 0.0f);
    pilot1.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot1.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot1);

    // Second pilot at relative position 200, H = 0.5
    ofdm::PilotSymbol pilot2;
    pilot2.subcarrier_index = first_active + 200;
#ifdef ATSC3_FIXED_POINT
    pilot2.reference_value = sample_t(32767, 0);
    pilot2.received_value = sample_t(16384, 0);
#else
    pilot2.reference_value = sample_t(1.0f, 0.0f);
    pilot2.received_value = sample_t(0.5f, 0.0f);
#endif
    pilot2.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot2);

    auto result = estimator.estimate(pilots, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_GT(result.h_estimate.size(), 150u);

    // Check interpolated value at midpoint (position 150)
    // Should be linearly interpolated: (1.0 + 0.5) / 2 = 0.75
#ifdef ATSC3_FIXED_POINT
    float h_mid = q15_to_float(result.h_estimate[150].real());
    EXPECT_NEAR(h_mid, 0.75f, 0.1f);
#else
    EXPECT_NEAR(result.h_estimate[150].real(), 0.75f, 0.01f);
#endif
}

TEST(ChannelEstimatorTest, InterpolationFlatChannel) {
    ChannelEstimator estimator;

    size_t first_active = estimator.get_first_active_carrier();

    // Create multiple pilots with same channel value
    std::vector<ofdm::PilotSymbol> pilots;
    for (size_t i = 0; i < 5; ++i) {
        ofdm::PilotSymbol pilot;
        pilot.subcarrier_index = first_active + 100 + i * 200;
#ifdef ATSC3_FIXED_POINT
        pilot.reference_value = sample_t(32767, 0);
        pilot.received_value = sample_t(32767, 0);
#else
        pilot.reference_value = sample_t(1.0f, 0.0f);
        pilot.received_value = sample_t(1.0f, 0.0f);
#endif
        pilot.type = ofdm::PilotType::SCATTERED;
        pilots.push_back(pilot);
    }

    auto result = estimator.estimate(pilots, 0);

    EXPECT_TRUE(result.is_valid);

    // All interpolated values should be ~1.0 for flat channel
    for (size_t k = 100; k < 900 && k < result.h_estimate.size(); ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(result.h_estimate[k].real()), 1.0f, 0.1f)
            << "Mismatch at subcarrier " << k;
#else
        EXPECT_NEAR(result.h_estimate[k].real(), 1.0f, 0.01f) << "Mismatch at subcarrier " << k;
#endif
    }
}

//==============================================================================
// Averaging Tests
//==============================================================================

TEST(ChannelEstimatorTest, AveragingDisabled) {
    ChannelEstimatorConfig config;
    config.fft_size = 8192;
    config.enable_averaging = false;
    ChannelEstimator estimator(config);

    size_t first_active = estimator.get_first_active_carrier();

    // Create pilot
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto result1 = estimator.estimate(pilots, 0);

    // Change pilot value
#ifdef ATSC3_FIXED_POINT
    pilots[0].received_value = sample_t(16384, 0);
#else
    pilots[0].received_value = sample_t(0.5f, 0.0f);
#endif

    auto result2 = estimator.estimate(pilots, 1);

    // Without averaging, estimate should immediately change
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(result1.h_estimate[100].real()), 1.0f, 0.1f);
    EXPECT_NEAR(q15_to_float(result2.h_estimate[100].real()), 0.5f, 0.1f);
#else
    EXPECT_NEAR(result1.h_estimate[100].real(), 1.0f, 0.01f);
    EXPECT_NEAR(result2.h_estimate[100].real(), 0.5f, 0.01f);
#endif
}

TEST(ChannelEstimatorTest, AveragingEnabled) {
    ChannelEstimatorConfig config;
    config.fft_size = 8192;
    config.enable_averaging = true;
    config.averaging_alpha = 0.5f;  // Fast convergence for testing
    ChannelEstimator estimator(config);

    size_t first_active = estimator.get_first_active_carrier();

    // Create pilot
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto result1 = estimator.estimate(pilots, 0);

    // Change pilot value
#ifdef ATSC3_FIXED_POINT
    pilots[0].received_value = sample_t(16384, 0);
#else
    pilots[0].received_value = sample_t(0.5f, 0.0f);
#endif

    auto result2 = estimator.estimate(pilots, 1);

    // With alpha=0.5, averaged = 0.5*0.5 + 0.5*1.0 = 0.75
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(result2.h_estimate[100].real()), 0.75f, 0.15f);
#else
    EXPECT_NEAR(result2.h_estimate[100].real(), 0.75f, 0.05f);
#endif
}

TEST(ChannelEstimatorTest, Reset) {
    ChannelEstimatorConfig config;
    config.fft_size = 8192;
    config.enable_averaging = true;
    ChannelEstimator estimator(config);

    size_t first_active = estimator.get_first_active_carrier();

    // Create pilot
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    estimator.estimate(pilots, 0);
    estimator.reset();

    // After reset, SNR should be 0
    EXPECT_FLOAT_EQ(estimator.get_estimated_snr_db(), 0.0f);
}

//==============================================================================
// SNR Estimation Tests
//==============================================================================

TEST(ChannelEstimatorTest, SnrEstimationHighSnr) {
    ChannelEstimator estimator;

    size_t first_active = estimator.get_first_active_carrier();

    // Create pilots with perfect channel (no noise)
    std::vector<ofdm::PilotSymbol> pilots;
    for (size_t i = 0; i < 20; ++i) {
        ofdm::PilotSymbol pilot;
        pilot.subcarrier_index = first_active + 100 + i * 50;
#ifdef ATSC3_FIXED_POINT
        pilot.reference_value = sample_t(32767, 0);
        pilot.received_value = sample_t(32767, 0);
#else
        pilot.reference_value = sample_t(1.0f, 0.0f);
        pilot.received_value = sample_t(1.0f, 0.0f);
#endif
        pilot.type = ofdm::PilotType::SCATTERED;
        pilots.push_back(pilot);
    }

    auto result = estimator.estimate(pilots, 0);

    // High SNR for perfect channel
    EXPECT_GT(result.estimated_snr_db, 30.0f);
}

TEST(ChannelEstimatorTest, SnrEstimationLowSnr) {
    ChannelEstimator estimator;

    size_t first_active = estimator.get_first_active_carrier();

    // Create pilots with noisy channel
    // Use imaginary component for noise to ensure it's seen as error
    std::vector<ofdm::PilotSymbol> pilots;
    for (size_t i = 0; i < 20; ++i) {
        ofdm::PilotSymbol pilot;
        pilot.subcarrier_index = first_active + 100 + i * 50;

        // For BPSK reference, noise on imaginary is pure error
        // Alternating +/- 0.3 imaginary gives ~10dB SNR
        float noise_im = (i % 2 == 0) ? 0.3f : -0.3f;
#ifdef ATSC3_FIXED_POINT
        pilot.reference_value = sample_t(32767, 0);
        // Received has unity real (signal) + imaginary noise
        int16_t sig_val = 32767;
        int16_t noise_val = float_to_q15(noise_im);
        pilot.received_value = sample_t(sig_val, noise_val);
#else
        pilot.reference_value = sample_t(1.0f, 0.0f);
        pilot.received_value = sample_t(1.0f, noise_im);
#endif
        pilot.type = ofdm::PilotType::SCATTERED;
        pilots.push_back(pilot);
    }

    auto result = estimator.estimate(pilots, 0);

    // Lower SNR due to noise (~10-15 dB for 0.3 RMS noise on unity signal)
    EXPECT_LT(result.estimated_snr_db, 20.0f);
    EXPECT_GT(result.estimated_snr_db, 0.0f);
}

//==============================================================================
// Phase Error Tests
//==============================================================================

TEST(ChannelEstimatorTest, MeanPhaseErrorZero) {
    ChannelEstimator estimator;

    size_t first_active = estimator.get_first_active_carrier();

    // No phase error
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto result = estimator.estimate(pilots, 0);

    EXPECT_NEAR(result.mean_phase_error, 0.0f, 0.01f);
}

TEST(ChannelEstimatorTest, MeanPhaseError90Degrees) {
    ChannelEstimator estimator;

    size_t first_active = estimator.get_first_active_carrier();

    // 90 degree phase error
    std::vector<ofdm::PilotSymbol> pilots;
    ofdm::PilotSymbol pilot;
    pilot.subcarrier_index = first_active + 100;
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, 32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif
    pilot.type = ofdm::PilotType::SCATTERED;
    pilots.push_back(pilot);

    auto result = estimator.estimate(pilots, 0);

    EXPECT_NEAR(result.mean_phase_error, M_PI / 2.0f, 0.01f);
}

//==============================================================================
// Safe Division Tests
//==============================================================================

TEST(ChannelEstimatorTest, SafeDivideBySmallValue) {
    sample_t a, b;

#ifdef ATSC3_FIXED_POINT
    a = sample_t(16384, 8192);  // 0.5 + 0.25j
    b = sample_t(1, 0);         // Very small
#else
    a = sample_t(0.5f, 0.25f);
    b = sample_t(1e-8f, 0.0f);  // Very small
#endif

    sample_t result = safe_complex_divide(a, b);

    // Should not crash, result should be clamped/zeroed
#ifdef ATSC3_FIXED_POINT
    // Either zero or a valid number
    EXPECT_LE(std::abs(result.real()), 32767);
    EXPECT_LE(std::abs(result.imag()), 32767);
#else
    EXPECT_TRUE(std::isfinite(result.real()));
    EXPECT_TRUE(std::isfinite(result.imag()));
#endif
}

TEST(ChannelEstimatorTest, SafeDivideByZero) {
    sample_t a, b;

#ifdef ATSC3_FIXED_POINT
    a = sample_t(16384, 8192);
    b = sample_t(0, 0);
#else
    a = sample_t(0.5f, 0.25f);
    b = sample_t(0.0f, 0.0f);
#endif

    sample_t result = safe_complex_divide(a, b);

    // Division by zero should return zero
#ifdef ATSC3_FIXED_POINT
    EXPECT_EQ(result.real(), 0);
    EXPECT_EQ(result.imag(), 0);
#else
    EXPECT_FLOAT_EQ(result.real(), 0.0f);
    EXPECT_FLOAT_EQ(result.imag(), 0.0f);
#endif
}

//==============================================================================
// Integration with PilotExtractor
//==============================================================================

TEST(ChannelEstimatorTest, IntegrationWithPilotExtractor) {
    // Create pilot extractor
    ofdm::PilotExtractorConfig pe_config;
    pe_config.fft_size = 8192;
    pe_config.pattern = ofdm::PilotPattern::PP3;
    ofdm::PilotExtractor extractor(pe_config);

    // Create channel estimator
    ChannelEstimatorConfig ce_config;
    ce_config.fft_size = 8192;
    ChannelEstimator estimator(ce_config);

    // Create FFT output that matches pilot reference values (unity channel)
    // For unity channel: received = reference (H = 1)
    std::vector<sample_t> fft_output(8192);
    for (size_t i = 0; i < 8192; ++i) {
        // Get the reference value at this position (BPSK: +1 or -1)
        auto ref = extractor.get_reference(i, 0);
        // For unity channel, received = reference
        fft_output[i] = ref;
    }

    // Extract pilots
    auto pilots = extractor.extract(fft_output.data(), 0);
    ASSERT_GT(pilots.size(), 10u);

    // Estimate channel
    auto result = estimator.estimate(pilots, 0);

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.h_estimate.size(), estimator.get_num_active_carriers());

    // All channel estimates should be unity (H = received / ref = ref / ref = 1)
    // Check a few estimates in the middle
    size_t mid = result.h_estimate.size() / 2;
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(result.h_estimate[mid].real()), 1.0f, 0.1f);
#else
    EXPECT_NEAR(result.h_estimate[mid].real(), 1.0f, 0.01f);
#endif

    // High SNR expected for perfect channel
    EXPECT_GT(result.estimated_snr_db, 20.0f);
}

//==============================================================================
// Reconfiguration Tests
//==============================================================================

TEST(ChannelEstimatorTest, Reconfigure) {
    ChannelEstimator estimator;

    EXPECT_EQ(estimator.get_config().fft_size, 8192u);

    ChannelEstimatorConfig new_config;
    new_config.fft_size = 16384;

    estimator.set_config(new_config);

    EXPECT_EQ(estimator.get_config().fft_size, 16384u);
    EXPECT_EQ(estimator.get_num_active_carriers(), 13825u);
}

}  // namespace
}  // namespace channel
}  // namespace atsc3
