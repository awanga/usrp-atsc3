// test_pilot_extractor.cc — Unit tests for lib/ofdm/pilot_extractor.h
//
// Tests pilot pattern extraction for all patterns PP1-PP8

#include "pilot_extractor.h"

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(PilotExtractorTest, DefaultConstruction) {
    PilotExtractor extractor;

    auto config = extractor.get_config();
    EXPECT_EQ(config.fft_size, 8192u);
    EXPECT_EQ(config.pattern, PilotPattern::PP3);
}

TEST(PilotExtractorTest, CustomConfig8K) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP1;

    PilotExtractor extractor(config);

    auto params = extractor.get_pattern_params();
    EXPECT_EQ(params.pattern, PilotPattern::PP1);
    EXPECT_EQ(params.dx, 3u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, CustomConfig16K) {
    PilotExtractorConfig config;
    config.fft_size = 16384;
    config.pattern = PilotPattern::PP5;

    PilotExtractor extractor(config);

    auto params = extractor.get_pattern_params();
    EXPECT_EQ(params.pattern, PilotPattern::PP5);
    EXPECT_EQ(params.dx, 12u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, CustomConfig32K) {
    PilotExtractorConfig config;
    config.fft_size = 32768;
    config.pattern = PilotPattern::PP7;

    PilotExtractor extractor(config);

    auto params = extractor.get_pattern_params();
    EXPECT_EQ(params.pattern, PilotPattern::PP7);
    EXPECT_EQ(params.dx, 24u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, InvalidFftSizeThrows) {
    PilotExtractorConfig config;
    config.fft_size = 4096;  // Invalid for pilot extraction
    config.pattern = PilotPattern::PP1;

    EXPECT_THROW(PilotExtractor extractor(config), std::invalid_argument);
}

//==============================================================================
// Pattern Parameters Tests
//==============================================================================

TEST(PilotExtractorTest, PatternParamsPP1) {
    auto params = get_pilot_pattern_params(PilotPattern::PP1, 8192);
    EXPECT_EQ(params.dx, 3u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, PatternParamsPP2) {
    auto params = get_pilot_pattern_params(PilotPattern::PP2, 8192);
    EXPECT_EQ(params.dx, 6u);
    EXPECT_EQ(params.dy, 2u);
}

TEST(PilotExtractorTest, PatternParamsPP3) {
    auto params = get_pilot_pattern_params(PilotPattern::PP3, 8192);
    EXPECT_EQ(params.dx, 6u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, PatternParamsPP4) {
    auto params = get_pilot_pattern_params(PilotPattern::PP4, 8192);
    EXPECT_EQ(params.dx, 12u);
    EXPECT_EQ(params.dy, 2u);
}

TEST(PilotExtractorTest, PatternParamsPP5) {
    auto params = get_pilot_pattern_params(PilotPattern::PP5, 8192);
    EXPECT_EQ(params.dx, 12u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, PatternParamsPP6) {
    auto params = get_pilot_pattern_params(PilotPattern::PP6, 8192);
    EXPECT_EQ(params.dx, 24u);
    EXPECT_EQ(params.dy, 2u);
}

TEST(PilotExtractorTest, PatternParamsPP7) {
    auto params = get_pilot_pattern_params(PilotPattern::PP7, 8192);
    EXPECT_EQ(params.dx, 24u);
    EXPECT_EQ(params.dy, 4u);
}

TEST(PilotExtractorTest, PatternParamsPP8) {
    auto params = get_pilot_pattern_params(PilotPattern::PP8, 8192);
    EXPECT_EQ(params.dx, 6u);
    EXPECT_EQ(params.dy, 16u);
}

//==============================================================================
// Pilot Count Tests
//==============================================================================

TEST(PilotExtractorTest, ContinualPilotCount8K) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    // 8K should have ~92 continual pilots
    size_t count = extractor.get_continual_pilot_count();
    EXPECT_GE(count, 80u);
    EXPECT_LE(count, 100u);
}

TEST(PilotExtractorTest, ScatteredPilotCountVariesBySymbol) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;  // Dy=4

    PilotExtractor extractor(config);

    // Scattered pilot count may vary by symbol index
    // but should be positive for all symbols in a Dy cycle
    auto params = extractor.get_pattern_params();
    for (size_t sym = 0; sym < params.dy; ++sym) {
        size_t count = extractor.get_scattered_pilot_count(sym);
        EXPECT_GT(count, 0u) << "No scattered pilots for symbol " << sym;
    }
}

TEST(PilotExtractorTest, ScatteredPilotCountPP1) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP1;  // Dx=3, Dy=4

    PilotExtractor extractor(config);

    // PP1 has high pilot density (Dx*Dy=12)
    // For 8K with ~6913 active carriers, expect ~576 scattered per symbol
    size_t count = extractor.get_scattered_pilot_count(0);
    EXPECT_GT(count, 500u);
    EXPECT_LT(count, 700u);
}

TEST(PilotExtractorTest, ScatteredPilotCountPP7) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP7;  // Dx=24, Dy=4

    PilotExtractor extractor(config);

    // PP7 has low pilot density (Dx*Dy=96)
    // For 8K with ~6913 active carriers, expect ~72 scattered per symbol
    size_t count = extractor.get_scattered_pilot_count(0);
    EXPECT_GT(count, 50u);
    EXPECT_LT(count, 100u);
}

//==============================================================================
// Pilot Position Tests
//==============================================================================

TEST(PilotExtractorTest, IsPilotAtEdges) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);
    auto params = extractor.get_pattern_params();

    // Edge pilots at first and last active carriers
    EXPECT_TRUE(extractor.is_pilot(params.first_active, 0));
    EXPECT_TRUE(extractor.is_pilot(params.last_active, 0));
}

TEST(PilotExtractorTest, IsContinualPilot) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    // Extract continual pilots and verify is_continual_pilot returns true
    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto continual = extractor.extract_continual(fft_output.data());

    for (const auto& pilot : continual) {
        EXPECT_TRUE(extractor.is_continual_pilot(pilot.subcarrier_index))
            << "Continual pilot at " << pilot.subcarrier_index << " not recognized";
    }
}

TEST(PilotExtractorTest, IsScatteredPilotPattern) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;  // Dx=6, Dy=4

    PilotExtractor extractor(config);
    auto params = extractor.get_pattern_params();

    // For symbol 0, scattered pilots at offset 0
    // Then every Dx*Dy = 24 subcarriers
    for (size_t k = params.first_active; k <= params.last_active; k += 24) {
        EXPECT_TRUE(extractor.is_scattered_pilot(k, 0))
            << "Expected scattered pilot at " << k << " for symbol 0";
    }

    // For symbol 1, offset is Dx = 6
    for (size_t k = params.first_active + 6; k <= params.last_active; k += 24) {
        EXPECT_TRUE(extractor.is_scattered_pilot(k, 1))
            << "Expected scattered pilot at " << k << " for symbol 1";
    }
}

//==============================================================================
// Extraction Tests
//==============================================================================

TEST(PilotExtractorTest, ExtractAllPilots) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    // Create FFT output with known values
    std::vector<sample_t> fft_output(8192);
    for (size_t i = 0; i < 8192; ++i) {
#ifdef ATSC3_FIXED_POINT
        fft_output[i] = sample_t(float_to_q15(0.5f), float_to_q15(0.25f));
#else
        fft_output[i] = sample_t(0.5f, 0.25f);
#endif
    }

    auto pilots = extractor.extract(fft_output.data(), 0);

    // Should have multiple pilots
    EXPECT_GT(pilots.size(), 50u);

    // All pilots should have received values matching input
    for (const auto& pilot : pilots) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(pilot.received_value.real()), 0.5f, 1e-3f);
        EXPECT_NEAR(q15_to_float(pilot.received_value.imag()), 0.25f, 1e-3f);
#else
        EXPECT_FLOAT_EQ(pilot.received_value.real(), 0.5f);
        EXPECT_FLOAT_EQ(pilot.received_value.imag(), 0.25f);
#endif
    }
}

TEST(PilotExtractorTest, ExtractScatteredOnly) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto scattered = extractor.extract_scattered(fft_output.data(), 0);

    // All should be scattered type
    for (const auto& pilot : scattered) {
        EXPECT_EQ(pilot.type, PilotType::SCATTERED);
    }

    // Count should match get_scattered_pilot_count
    EXPECT_EQ(scattered.size(), extractor.get_scattered_pilot_count(0));
}

TEST(PilotExtractorTest, ExtractContinualOnly) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto continual = extractor.extract_continual(fft_output.data());

    // All should be continual type
    for (const auto& pilot : continual) {
        EXPECT_EQ(pilot.type, PilotType::CONTINUAL);
    }

    // Count should match get_continual_pilot_count
    EXPECT_EQ(continual.size(), extractor.get_continual_pilot_count());
}

TEST(PilotExtractorTest, ExtractEdgeOnly) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);
    auto params = extractor.get_pattern_params();

    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto edge = extractor.extract_edge(fft_output.data());

    // Should have exactly 2 edge pilots
    EXPECT_EQ(edge.size(), 2u);

    // All should be edge type
    for (const auto& pilot : edge) {
        EXPECT_EQ(pilot.type, PilotType::EDGE);
    }

    // Verify positions
    EXPECT_EQ(edge[0].subcarrier_index, params.first_active);
    EXPECT_EQ(edge[1].subcarrier_index, params.last_active);
}

//==============================================================================
// Reference Value Tests
//==============================================================================

TEST(PilotExtractorTest, ReferenceValueIsBPSK) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto pilots = extractor.extract(fft_output.data(), 0);

    // All pilot reference values should be BPSK (+1 or -1 real, 0 imag)
    for (const auto& pilot : pilots) {
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(pilot.reference_value.real());
        float im = q15_to_float(pilot.reference_value.imag());
        EXPECT_TRUE(std::abs(re - 1.0f) < 0.01f || std::abs(re + 1.0f) < 0.01f)
            << "Reference real part should be +/-1, got " << re;
        EXPECT_NEAR(im, 0.0f, 0.01f) << "Reference imag part should be 0, got " << im;
#else
        EXPECT_TRUE(pilot.reference_value.real() == 1.0f || pilot.reference_value.real() == -1.0f)
            << "Reference real part should be +/-1";
        EXPECT_FLOAT_EQ(pilot.reference_value.imag(), 0.0f) << "Reference imag part should be 0";
#endif
    }
}

TEST(PilotExtractorTest, GetReferenceValue) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);
    auto params = extractor.get_pattern_params();

    // Get reference at a few positions
    auto ref1 = extractor.get_reference(params.first_active, 0);
    auto ref2 = extractor.get_reference(params.first_active + 100, 0);

#ifdef ATSC3_FIXED_POINT
    // Should be BPSK
    EXPECT_TRUE(std::abs(q15_to_float(ref1.real())) > 0.9f);
    EXPECT_TRUE(std::abs(q15_to_float(ref2.real())) > 0.9f);
#else
    EXPECT_TRUE(std::abs(ref1.real()) == 1.0f);
    EXPECT_TRUE(std::abs(ref2.real()) == 1.0f);
#endif
}

//==============================================================================
// Channel Estimation Tests
//==============================================================================

TEST(PilotSymbolTest, EstimateChannelUnity) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = +1+j0 -> Channel = +1+j0
#ifdef ATSC3_FIXED_POINT
    // Use 32767 (max Q15) instead of float_to_q15(1.0f) to avoid overflow
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 1.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 0.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 1.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 0.0f, 1e-6f);
#endif
}

TEST(PilotSymbolTest, EstimateChannelWithPhase) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0+j1 -> Channel = 0+j1 (90 deg rotation)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, 32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 0.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 1.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 0.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 1.0f, 1e-6f);
#endif
}

TEST(PilotSymbolTest, EstimateChannelNegativeRef) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = -1, Received = -1+j0 -> Channel = +1+j0
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(-32767, 0);
    pilot.received_value = sample_t(-32767, 0);
#else
    pilot.reference_value = sample_t(-1.0f, 0.0f);
    pilot.received_value = sample_t(-1.0f, 0.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 1.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 0.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 1.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 0.0f, 1e-6f);
#endif
}

//==============================================================================
// Phase Error Tests
//==============================================================================

TEST(PilotSymbolTest, PhaseErrorZero) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = +1+j0 -> No phase error
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, 0.0, 1e-6);
}

TEST(PilotSymbolTest, PhaseError90Degrees) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0+j1 -> 90 degree phase error (pi/2)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, 32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, M_PI / 2.0, 1e-5);
}

TEST(PilotSymbolTest, PhaseErrorMinus90Degrees) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0-j1 -> -90 degree phase error (-pi/2)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, -32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, -1.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, -M_PI / 2.0, 1e-5);
}

//==============================================================================
// Reconfiguration Tests
//==============================================================================

TEST(PilotExtractorTest, Reconfigure) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    auto params1 = extractor.get_pattern_params();
    EXPECT_EQ(params1.dx, 6u);
    EXPECT_EQ(params1.dy, 4u);

    // Reconfigure to different pattern
    PilotExtractorConfig new_config;
    new_config.fft_size = 8192;
    new_config.pattern = PilotPattern::PP1;

    extractor.set_config(new_config);

    auto params2 = extractor.get_pattern_params();
    EXPECT_EQ(params2.dx, 3u);
    EXPECT_EQ(params2.dy, 4u);
}

TEST(PilotExtractorTest, ReconfigureFftSize) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    size_t cp_count_8k = extractor.get_continual_pilot_count();

    // Reconfigure to 16K
    PilotExtractorConfig new_config;
    new_config.fft_size = 16384;
    new_config.pattern = PilotPattern::PP3;

    extractor.set_config(new_config);

    size_t cp_count_16k = extractor.get_continual_pilot_count();

    // 16K should have more continual pilots
    EXPECT_GT(cp_count_16k, cp_count_8k);
}

//==============================================================================
// Multi-FFT Size Tests
//==============================================================================

TEST(PilotExtractorTest, PilotCountsScale16K) {
    PilotExtractorConfig config8k;
    config8k.fft_size = 8192;
    config8k.pattern = PilotPattern::PP3;

    PilotExtractorConfig config16k;
    config16k.fft_size = 16384;
    config16k.pattern = PilotPattern::PP3;

    PilotExtractor ext8k(config8k);
    PilotExtractor ext16k(config16k);

    // 16K should have roughly 2x as many scattered pilots
    size_t sp_8k = ext8k.get_scattered_pilot_count(0);
    size_t sp_16k = ext16k.get_scattered_pilot_count(0);

    EXPECT_GT(sp_16k, sp_8k);
    EXPECT_LT(sp_16k, sp_8k * 3);  // Should scale roughly linearly
}

TEST(PilotExtractorTest, PilotCountsScale32K) {
    PilotExtractorConfig config8k;
    config8k.fft_size = 8192;
    config8k.pattern = PilotPattern::PP3;

    PilotExtractorConfig config32k;
    config32k.fft_size = 32768;
    config32k.pattern = PilotPattern::PP3;

    PilotExtractor ext8k(config8k);
    PilotExtractor ext32k(config32k);

    // 32K should have roughly 4x as many scattered pilots
    size_t sp_8k = ext8k.get_scattered_pilot_count(0);
    size_t sp_32k = ext32k.get_scattered_pilot_count(0);

    EXPECT_GT(sp_32k, sp_8k * 2);
    EXPECT_LT(sp_32k, sp_8k * 6);
}

//==============================================================================
// All Patterns Work Test
//==============================================================================

TEST(PilotExtractorTest, AllPatternsWork8K) {
    std::vector<PilotPattern> patterns = {PilotPattern::PP1, PilotPattern::PP2, PilotPattern::PP3,
                                          PilotPattern::PP4, PilotPattern::PP5, PilotPattern::PP6,
                                          PilotPattern::PP7, PilotPattern::PP8};

    for (auto pattern : patterns) {
        PilotExtractorConfig config;
        config.fft_size = 8192;
        config.pattern = pattern;

        EXPECT_NO_THROW({
            PilotExtractor extractor(config);

            std::vector<sample_t> fft_output(8192, sample_t(0, 0));
            auto pilots = extractor.extract(fft_output.data(), 0);

            // Should have some pilots
            EXPECT_GT(pilots.size(), 0u)
                << "No pilots extracted for pattern " << static_cast<int>(pattern);

            // Should have continual pilots
            EXPECT_GT(extractor.get_continual_pilot_count(), 0u)
                << "No continual pilots for pattern " << static_cast<int>(pattern);

            // Should have scattered pilots
            EXPECT_GT(extractor.get_scattered_pilot_count(0), 0u)
                << "No scattered pilots for pattern " << static_cast<int>(pattern);
        }) << "Exception for pattern "
           << static_cast<int>(pattern);
    }
}

//==============================================================================
// Sorted and Unique Output Test
//==============================================================================

TEST(PilotExtractorTest, OutputIsSortedAndUnique) {
    PilotExtractorConfig config;
    config.fft_size = 8192;
    config.pattern = PilotPattern::PP3;

    PilotExtractor extractor(config);

    std::vector<sample_t> fft_output(8192, sample_t(0, 0));
    auto pilots = extractor.extract(fft_output.data(), 0);

    // Verify sorted
    for (size_t i = 1; i < pilots.size(); ++i) {
        EXPECT_LT(pilots[i - 1].subcarrier_index, pilots[i].subcarrier_index)
            << "Pilots not sorted at index " << i;
    }
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
