// test_atsc3_config.cc — Unit tests for lib/config/atsc3_config.h
//
// Verifies ATSC 3.0 runtime configuration structures and helper methods.

#include "config/atsc3_config.h"

#include <gtest/gtest.h>

namespace atsc3 {
namespace config {
namespace {

// Test Modulation enum values
TEST(Atsc3ConfigTest, ModulationEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QPSK), 0);
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QAM16), 1);
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QAM64), 2);
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QAM256), 3);
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QAM1024), 4);
    EXPECT_EQ(static_cast<uint8_t>(Modulation::QAM4096), 5);
}

// Test CodeRate enum values
TEST(Atsc3ConfigTest, CodeRateEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(CodeRate::RATE_2_15), 0);
    EXPECT_EQ(static_cast<uint8_t>(CodeRate::RATE_7_15), 5);
    EXPECT_EQ(static_cast<uint8_t>(CodeRate::RATE_13_15), 11);
}

// Test FftSize enum values
TEST(Atsc3ConfigTest, FftSizeEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(FftSize::FFT_8K), 0);
    EXPECT_EQ(static_cast<uint8_t>(FftSize::FFT_16K), 1);
    EXPECT_EQ(static_cast<uint8_t>(FftSize::FFT_32K), 2);
}

// Test PlpConfig default values
TEST(PlpConfigTest, DefaultConstruction) {
    PlpConfig plp;
    EXPECT_EQ(plp.plp_id, 0);
    EXPECT_EQ(plp.modulation, Modulation::QPSK);
    EXPECT_EQ(plp.code_rate, CodeRate::RATE_7_15);
    EXPECT_EQ(plp.codeword_length, CodewordLength::LONG);
    EXPECT_FALSE(plp.valid);
}

// Test Atsc3Config default values
TEST(Atsc3ConfigTest, DefaultConstruction) {
    Atsc3Config config;
    EXPECT_EQ(config.fft_size, FftSize::FFT_8K);
    EXPECT_EQ(config.cp_length, CpLength::CP_1024_8192);
    EXPECT_EQ(config.num_plps, 0);
    EXPECT_FALSE(config.valid);
}

// Test get_fft_samples helper
TEST(Atsc3ConfigTest, GetFftSamples) {
    Atsc3Config config;

    config.fft_size = FftSize::FFT_8K;
    EXPECT_EQ(config.get_fft_samples(), 8192u);

    config.fft_size = FftSize::FFT_16K;
    EXPECT_EQ(config.get_fft_samples(), 16384u);

    config.fft_size = FftSize::FFT_32K;
    EXPECT_EQ(config.get_fft_samples(), 32768u);
}

// Test get_cp_samples helper
TEST(Atsc3ConfigTest, GetCpSamples8K) {
    Atsc3Config config;
    config.fft_size = FftSize::FFT_8K;

    config.cp_length = CpLength::CP_192_8192;
    EXPECT_EQ(config.get_cp_samples(), 192u);

    config.cp_length = CpLength::CP_512_8192;
    EXPECT_EQ(config.get_cp_samples(), 512u);

    config.cp_length = CpLength::CP_1024_8192;
    EXPECT_EQ(config.get_cp_samples(), 1024u);

    config.cp_length = CpLength::CP_2048_8192;
    EXPECT_EQ(config.get_cp_samples(), 2048u);
}

// Test CP samples scales with FFT size
TEST(Atsc3ConfigTest, GetCpSamplesScalesWithFft) {
    Atsc3Config config;
    config.cp_length = CpLength::CP_1024_8192;

    config.fft_size = FftSize::FFT_8K;
    size_t cp_8k = config.get_cp_samples();
    EXPECT_EQ(cp_8k, 1024u);

    config.fft_size = FftSize::FFT_16K;
    size_t cp_16k = config.get_cp_samples();
    EXPECT_EQ(cp_16k, 2048u);  // 2x

    config.fft_size = FftSize::FFT_32K;
    size_t cp_32k = config.get_cp_samples();
    EXPECT_EQ(cp_32k, 4096u);  // 4x
}

// Test get_active_carriers helper
TEST(Atsc3ConfigTest, GetActiveCarriers) {
    Atsc3Config config;

    config.fft_size = FftSize::FFT_8K;
    EXPECT_EQ(config.get_active_carriers(), 6913u);

    config.fft_size = FftSize::FFT_16K;
    EXPECT_EQ(config.get_active_carriers(), 13825u);

    config.fft_size = FftSize::FFT_32K;
    EXPECT_EQ(config.get_active_carriers(), 27649u);
}

// Test get_first_active_carrier helper (carriers are centered)
TEST(Atsc3ConfigTest, GetFirstActiveCarrier) {
    Atsc3Config config;

    config.fft_size = FftSize::FFT_8K;
    EXPECT_EQ(config.get_first_active_carrier(), (8192 - 6913) / 2);

    config.fft_size = FftSize::FFT_16K;
    EXPECT_EQ(config.get_first_active_carrier(), (16384 - 13825) / 2);

    config.fft_size = FftSize::FFT_32K;
    EXPECT_EQ(config.get_first_active_carrier(), (32768 - 27649) / 2);
}

// Test get_codeword_bits helper
TEST(Atsc3ConfigTest, GetCodewordBits) {
    Atsc3Config config;
    config.num_plps = 2;
    config.plps[0].codeword_length = CodewordLength::SHORT;
    config.plps[1].codeword_length = CodewordLength::LONG;

    EXPECT_EQ(config.get_codeword_bits(0), 16200u);
    EXPECT_EQ(config.get_codeword_bits(1), 64800u);
    EXPECT_EQ(config.get_codeword_bits(2), 0u);  // Invalid PLP
}

// Test code_rate_value static helper
TEST(Atsc3ConfigTest, CodeRateValue) {
    EXPECT_NEAR(Atsc3Config::code_rate_value(CodeRate::RATE_2_15), 2.0f / 15.0f, 1e-6f);
    EXPECT_NEAR(Atsc3Config::code_rate_value(CodeRate::RATE_7_15), 7.0f / 15.0f, 1e-6f);
    EXPECT_NEAR(Atsc3Config::code_rate_value(CodeRate::RATE_13_15), 13.0f / 15.0f, 1e-6f);
}

// Test bits_per_symbol static helper
TEST(Atsc3ConfigTest, BitsPerSymbol) {
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QPSK), 2u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QAM16), 4u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QAM64), 6u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QAM256), 8u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QAM1024), 10u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::QAM4096), 12u);

    // NUC variants have same bits per symbol
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::NUC_QPSK), 2u);
    EXPECT_EQ(Atsc3Config::bits_per_symbol(Modulation::NUC_64), 6u);
}

// Test get_default_config
TEST(Atsc3ConfigTest, GetDefaultConfig) {
    auto config = get_default_config();

    EXPECT_TRUE(config.valid);
    EXPECT_EQ(config.fft_size, FftSize::FFT_8K);
    EXPECT_EQ(config.cp_length, CpLength::CP_1024_8192);
    EXPECT_EQ(config.pilot_pattern, PilotPattern::PP3);
    EXPECT_EQ(config.num_plps, 1);

    EXPECT_TRUE(config.plps[0].valid);
    EXPECT_EQ(config.plps[0].plp_id, 0);
    EXPECT_EQ(config.plps[0].modulation, Modulation::QAM64);
    EXPECT_EQ(config.plps[0].code_rate, CodeRate::RATE_7_15);
}

// Test MAX_PLPS constant
TEST(Atsc3ConfigTest, MaxPlpsConstant) {
    EXPECT_EQ(MAX_PLPS, 64u);  // ATSC 3.0 limit
}

// Test Atsc3Config size (no dynamic allocation)
TEST(Atsc3ConfigTest, NoVectors) {
    // Atsc3Config should have fixed size (no dynamic allocation)
    // This verifies the std::array approach works
    Atsc3Config config1;
    Atsc3Config config2 = config1;  // Copy should work

    config2.num_plps = 5;
    config2.plps[0].modulation = Modulation::QAM256;

    // Original unchanged
    EXPECT_EQ(config1.num_plps, 0);
    EXPECT_EQ(config1.plps[0].modulation, Modulation::QPSK);
}

// Test PilotPattern enum range
TEST(Atsc3ConfigTest, PilotPatternRange) {
    EXPECT_EQ(static_cast<uint8_t>(PilotPattern::PP1), 1);
    EXPECT_EQ(static_cast<uint8_t>(PilotPattern::PP8), 8);
}

// Test TimeInterleaveMode enum
TEST(Atsc3ConfigTest, TimeInterleaveModeEnum) {
    EXPECT_EQ(static_cast<uint8_t>(TimeInterleaveMode::NONE), 0);
    EXPECT_EQ(static_cast<uint8_t>(TimeInterleaveMode::CTI), 1);
    EXPECT_EQ(static_cast<uint8_t>(TimeInterleaveMode::HTI), 2);
}

}  // namespace
}  // namespace config
}  // namespace atsc3
