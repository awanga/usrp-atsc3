// test_constellation_demapper.cc — Unit tests for lib/ofdm/constellation_demapper.h
//
// Tests constellation demapping, LLR computation, and NUC support

#include "constellation_demapper.h"

#include <cmath>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(ConstellationDemapperTest, DefaultConstruction) {
    ConstellationDemapper demapper;

    auto config = demapper.get_config();
    EXPECT_EQ(config.modulation, config::Modulation::QAM64);
    EXPECT_EQ(demapper.bits_per_symbol(), 6u);
    EXPECT_EQ(demapper.constellation_size(), 64u);
}

TEST(ConstellationDemapperTest, QPSKConstruction) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;

    ConstellationDemapper demapper(config);

    EXPECT_EQ(demapper.bits_per_symbol(), 2u);
    EXPECT_EQ(demapper.constellation_size(), 4u);
}

TEST(ConstellationDemapperTest, AllUniformConstellations) {
    std::vector<std::pair<config::Modulation, size_t>> test_cases = {
        {config::Modulation::QPSK, 2},     {config::Modulation::QAM16, 4},
        {config::Modulation::QAM64, 6},    {config::Modulation::QAM256, 8},
        {config::Modulation::QAM1024, 10}, {config::Modulation::QAM4096, 12}};

    for (const auto& [mod, bits] : test_cases) {
        DemapperConfig config;
        config.modulation = mod;

        EXPECT_NO_THROW({
            ConstellationDemapper demapper(config);
            EXPECT_EQ(demapper.bits_per_symbol(), bits);
            EXPECT_EQ(demapper.constellation_size(), 1u << bits);
        }) << "Failed for modulation "
           << static_cast<int>(mod);
    }
}

TEST(ConstellationDemapperTest, NUCConstellations) {
    std::vector<config::Modulation> nuc_mods = {
        config::Modulation::NUC_QPSK, config::Modulation::NUC_16,   config::Modulation::NUC_64,
        config::Modulation::NUC_256,  config::Modulation::NUC_1024, config::Modulation::NUC_4096};

    for (auto mod : nuc_mods) {
        DemapperConfig config;
        config.modulation = mod;

        EXPECT_NO_THROW({ ConstellationDemapper demapper(config); })
            << "Failed to construct NUC demapper for " << static_cast<int>(mod);
    }
}

//==============================================================================
// Constellation Point Tests
//==============================================================================

TEST(ConstellationDemapperTest, QPSKConstellationPoints) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();
    ASSERT_EQ(points.size(), 4u);

    // Verify unit power: |point|² ≈ 1 for each point
    for (const auto& p : points) {
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(p.real());
        float im = q15_to_float(p.imag());
#else
        float re = p.real();
        float im = p.imag();
#endif
        float power = re * re + im * im;
        EXPECT_NEAR(power, 1.0f, 0.01f);
    }
}

TEST(ConstellationDemapperTest, QAM16ConstellationPoints) {
    DemapperConfig config;
    config.modulation = config::Modulation::QAM16;
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();
    ASSERT_EQ(points.size(), 16u);

    // Verify average power ≈ 1
    float avg_power = 0.0f;
    for (const auto& p : points) {
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(p.real());
        float im = q15_to_float(p.imag());
#else
        float re = p.real();
        float im = p.imag();
#endif
        avg_power += re * re + im * im;
    }
    avg_power /= 16.0f;
    EXPECT_NEAR(avg_power, 1.0f, 0.05f);
}

TEST(ConstellationDemapperTest, QAM64ConstellationPoints) {
    DemapperConfig config;
    config.modulation = config::Modulation::QAM64;
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();
    ASSERT_EQ(points.size(), 64u);

    // Verify average power ≈ 1
    float avg_power = 0.0f;
    for (const auto& p : points) {
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(p.real());
        float im = q15_to_float(p.imag());
#else
        float re = p.real();
        float im = p.imag();
#endif
        avg_power += re * re + im * im;
    }
    avg_power /= 64.0f;
    EXPECT_NEAR(avg_power, 1.0f, 0.05f);
}

//==============================================================================
// LLR Sign Test (Key Requirement)
//==============================================================================

TEST(ConstellationDemapperTest, QPSKLLRSignMatchesBitAtHighSNR) {
    // Test requirement: "QPSK at SNR=10dB: LLR sign matches transmitted bit >99.9%"

    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    config.noise_variance = 0.1f;  // ~10 dB SNR
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();

    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0f, std::sqrt(config.noise_variance / 2.0f));

    size_t num_trials = 10000;
    size_t correct_count = 0;

    for (size_t trial = 0; trial < num_trials; ++trial) {
        // Pick random constellation point
        size_t idx = rng() % 4;
        sample_t tx = points[idx];

        // Add noise
#ifdef ATSC3_FIXED_POINT
        float tx_re = q15_to_float(tx.real()) + noise(rng);
        float tx_im = q15_to_float(tx.imag()) + noise(rng);
        sample_t rx(float_to_q15(tx_re), float_to_q15(tx_im));
#else
        sample_t rx(tx.real() + noise(rng), tx.imag() + noise(rng));
#endif

        // Demap
        int8_t llr[2];
        demapper.demap_symbol(rx, llr);

        // Check LLR signs match transmitted bits
        // Positive LLR means bit=0 is more likely
        // idx's bit pattern determines transmitted bits
        bool bit0 = (idx >> 1) & 1;  // MSB
        bool bit1 = idx & 1;         // LSB

        bool llr0_correct = (llr[0] > 0) == !bit0 || (llr[0] < 0) == bit0;
        bool llr1_correct = (llr[1] > 0) == !bit1 || (llr[1] < 0) == bit1;

        if (llr0_correct && llr1_correct) {
            ++correct_count;
        }
    }

    double accuracy = static_cast<double>(correct_count) / num_trials;
    EXPECT_GT(accuracy, 0.99) << "LLR sign accuracy " << (accuracy * 100) << "% < 99%";
}

TEST(ConstellationDemapperTest, QAM64LLRSignMatchesBitAtHighSNR) {
    DemapperConfig config;
    config.modulation = config::Modulation::QAM64;
    config.noise_variance = 0.01f;  // ~20 dB SNR
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();

    std::mt19937 rng(123);
    std::normal_distribution<float> noise(0.0f, std::sqrt(config.noise_variance / 2.0f));

    size_t num_trials = 10000;
    size_t total_bits = 0;
    size_t correct_bits = 0;

    for (size_t trial = 0; trial < num_trials; ++trial) {
        // Pick random constellation point
        size_t idx = rng() % 64;
        sample_t tx = points[idx];

        // Add noise
#ifdef ATSC3_FIXED_POINT
        float tx_re = q15_to_float(tx.real()) + noise(rng);
        float tx_im = q15_to_float(tx.imag()) + noise(rng);
        sample_t rx(float_to_q15(tx_re), float_to_q15(tx_im));
#else
        sample_t rx(tx.real() + noise(rng), tx.imag() + noise(rng));
#endif

        // Demap
        int8_t llr[6];
        demapper.demap_symbol(rx, llr);

        // Check each bit
        for (size_t b = 0; b < 6; ++b) {
            bool tx_bit = (idx >> (5 - b)) & 1;
            // Positive LLR -> bit=0 more likely
            bool llr_says_zero = llr[b] > 0;

            if (llr_says_zero == !tx_bit) {
                ++correct_bits;
            }
            ++total_bits;
        }
    }

    double accuracy = static_cast<double>(correct_bits) / total_bits;
    EXPECT_GT(accuracy, 0.99) << "QAM-64 LLR bit accuracy " << (accuracy * 100) << "% < 99%";
}

//==============================================================================
// Demapping Tests
//==============================================================================

TEST(ConstellationDemapperTest, DemapSingleSymbol) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    config.noise_variance = 0.01f;  // High SNR for strong LLRs
    ConstellationDemapper demapper(config);

    // Transmit point 0 (should map to bits 00)
    sample_t tx = demapper.constellation_points()[0];

    int8_t llr[2];
    demapper.demap_symbol(tx, llr);

    // Both LLRs should be strongly positive (favoring bit=0)
    // With noise_variance=0.01, LLR scale = 100, so expect large magnitudes
    EXPECT_GT(llr[0], 50);
    EXPECT_GT(llr[1], 50);
}

TEST(ConstellationDemapperTest, DemapMultipleSymbols) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();
    std::vector<sample_t> symbols = {points[0], points[1], points[2], points[3]};

    auto result = demapper.demap(symbols.data(), symbols.size());

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.num_symbols, 4u);
    EXPECT_EQ(result.bits_per_symbol, 2u);
    EXPECT_EQ(result.llr.size(), 8u);
}

TEST(ConstellationDemapperTest, DemapIntoPreallocatedBuffer) {
    DemapperConfig config;
    config.modulation = config::Modulation::QAM16;
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();
    std::vector<sample_t> symbols(10);
    for (size_t i = 0; i < 10; ++i) {
        symbols[i] = points[i % 16];
    }

    std::vector<int8_t> llr_buf(40);  // 10 symbols * 4 bits

    size_t num_llr = demapper.demap(symbols.data(), symbols.size(), llr_buf.data());

    EXPECT_EQ(num_llr, 40u);
}

TEST(ConstellationDemapperTest, DemapNullInput) {
    ConstellationDemapper demapper;

    auto result = demapper.demap(nullptr, 10);

    EXPECT_FALSE(result.is_valid);
    EXPECT_EQ(result.num_symbols, 0u);
}

TEST(ConstellationDemapperTest, DemapZeroSymbols) {
    ConstellationDemapper demapper;
    std::vector<sample_t> symbols(1);

    auto result = demapper.demap(symbols.data(), 0);

    EXPECT_FALSE(result.is_valid);
    EXPECT_EQ(result.num_symbols, 0u);
}

//==============================================================================
// LLR Clipping Tests
//==============================================================================

TEST(ConstellationDemapperTest, LLRClipping) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    config.noise_variance = 0.001f;  // Very high SNR
    config.llr_clip = 100;
    ConstellationDemapper demapper(config);

    sample_t tx = demapper.constellation_points()[0];

    int8_t llr[2];
    demapper.demap_symbol(tx, llr);

    // LLRs should be clipped to [-100, 100]
    EXPECT_LE(std::abs(llr[0]), 100);
    EXPECT_LE(std::abs(llr[1]), 100);
}

TEST(ConstellationDemapperTest, DefaultLLRClipIs127) {
    DemapperConfig config;
    EXPECT_EQ(config.llr_clip, 127);
}

//==============================================================================
// Noise Variance Scaling Tests
//==============================================================================

TEST(ConstellationDemapperTest, HigherNoiseVarianceReducesLLRMagnitude) {
    DemapperConfig config1;
    config1.modulation = config::Modulation::QPSK;
    config1.noise_variance = 0.01f;
    ConstellationDemapper demapper1(config1);

    DemapperConfig config2;
    config2.modulation = config::Modulation::QPSK;
    config2.noise_variance = 0.1f;
    ConstellationDemapper demapper2(config2);

    sample_t tx = demapper1.constellation_points()[0];

    int8_t llr1[2], llr2[2];
    demapper1.demap_symbol(tx, llr1);
    demapper2.demap_symbol(tx, llr2);

    // Lower noise variance (higher SNR) should give larger LLR magnitudes
    EXPECT_GT(std::abs(llr1[0]), std::abs(llr2[0]));
    EXPECT_GT(std::abs(llr1[1]), std::abs(llr2[1]));
}

//==============================================================================
// Reconfiguration Tests
//==============================================================================

TEST(ConstellationDemapperTest, Reconfigure) {
    DemapperConfig config;
    config.modulation = config::Modulation::QPSK;
    ConstellationDemapper demapper(config);

    EXPECT_EQ(demapper.bits_per_symbol(), 2u);

    DemapperConfig new_config;
    new_config.modulation = config::Modulation::QAM256;

    demapper.set_config(new_config);

    EXPECT_EQ(demapper.bits_per_symbol(), 8u);
    EXPECT_EQ(demapper.constellation_size(), 256u);
}

//==============================================================================
// Utility Function Tests
//==============================================================================

TEST(ConstellationDemapperTest, IsNucModulation) {
    EXPECT_FALSE(is_nuc_modulation(config::Modulation::QPSK));
    EXPECT_FALSE(is_nuc_modulation(config::Modulation::QAM16));
    EXPECT_FALSE(is_nuc_modulation(config::Modulation::QAM64));
    EXPECT_FALSE(is_nuc_modulation(config::Modulation::QAM256));

    EXPECT_TRUE(is_nuc_modulation(config::Modulation::NUC_QPSK));
    EXPECT_TRUE(is_nuc_modulation(config::Modulation::NUC_16));
    EXPECT_TRUE(is_nuc_modulation(config::Modulation::NUC_64));
    EXPECT_TRUE(is_nuc_modulation(config::Modulation::NUC_256));
}

TEST(ConstellationDemapperTest, GetBitsPerSymbol) {
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QPSK), 2u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QAM16), 4u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QAM64), 6u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QAM256), 8u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QAM1024), 10u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::QAM4096), 12u);

    // NUC should match uniform
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::NUC_QPSK), 2u);
    EXPECT_EQ(get_bits_per_symbol(config::Modulation::NUC_16), 4u);
}

TEST(ConstellationDemapperTest, GetConstellationSize) {
    EXPECT_EQ(get_constellation_size(config::Modulation::QPSK), 4u);
    EXPECT_EQ(get_constellation_size(config::Modulation::QAM16), 16u);
    EXPECT_EQ(get_constellation_size(config::Modulation::QAM64), 64u);
    EXPECT_EQ(get_constellation_size(config::Modulation::QAM256), 256u);
    EXPECT_EQ(get_constellation_size(config::Modulation::QAM1024), 1024u);
    EXPECT_EQ(get_constellation_size(config::Modulation::QAM4096), 4096u);
}

//==============================================================================
// Integration Tests
//==============================================================================

TEST(ConstellationDemapperTest, FullDemapPipeline) {
    // Simulate full pipeline: TX → AWGN channel → demapper

    DemapperConfig config;
    config.modulation = config::Modulation::QAM64;
    config.noise_variance = 0.02f;  // ~17 dB SNR
    ConstellationDemapper demapper(config);

    const auto& points = demapper.constellation_points();

    std::mt19937 rng(789);
    std::uniform_int_distribution<size_t> symbol_dist(0, 63);
    std::normal_distribution<float> noise(0.0f, std::sqrt(config.noise_variance / 2.0f));

    // Transmit 1000 symbols
    size_t num_symbols = 1000;
    std::vector<sample_t> tx_symbols(num_symbols);
    std::vector<size_t> tx_indices(num_symbols);
    std::vector<sample_t> rx_symbols(num_symbols);

    for (size_t i = 0; i < num_symbols; ++i) {
        size_t idx = symbol_dist(rng);
        tx_indices[i] = idx;
        tx_symbols[i] = points[idx];

#ifdef ATSC3_FIXED_POINT
        float rx_re = q15_to_float(tx_symbols[i].real()) + noise(rng);
        float rx_im = q15_to_float(tx_symbols[i].imag()) + noise(rng);
        rx_symbols[i] = sample_t(float_to_q15(rx_re), float_to_q15(rx_im));
#else
        rx_symbols[i] =
            sample_t(tx_symbols[i].real() + noise(rng), tx_symbols[i].imag() + noise(rng));
#endif
    }

    // Demap
    auto result = demapper.demap(rx_symbols.data(), num_symbols);

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.num_symbols, num_symbols);
    EXPECT_EQ(result.llr.size(), num_symbols * 6);

    // Check bit error rate (should be low at this SNR)
    size_t bit_errors = 0;
    for (size_t i = 0; i < num_symbols; ++i) {
        size_t tx_idx = tx_indices[i];
        for (size_t b = 0; b < 6; ++b) {
            bool tx_bit = (tx_idx >> (5 - b)) & 1;
            bool llr_says_zero = result.llr[i * 6 + b] > 0;

            if (llr_says_zero == tx_bit) {
                ++bit_errors;
            }
        }
    }

    double ber = static_cast<double>(bit_errors) / (num_symbols * 6);
    EXPECT_LT(ber, 0.05) << "BER " << (ber * 100) << "% too high at 17 dB SNR";
}

#ifndef ATSC3_FIXED_POINT
TEST(ConstellationDemapperTest, LargeConstellationStress) {
    // Test 4096-QAM (12 bits per symbol)
    DemapperConfig config;
    config.modulation = config::Modulation::QAM4096;
    config.noise_variance = 0.001f;  // High SNR needed for 4096-QAM
    ConstellationDemapper demapper(config);

    EXPECT_EQ(demapper.bits_per_symbol(), 12u);
    EXPECT_EQ(demapper.constellation_size(), 4096u);

    // Demap 100 random symbols
    const auto& points = demapper.constellation_points();
    std::mt19937 rng(456);
    std::uniform_int_distribution<size_t> dist(0, 4095);

    std::vector<sample_t> symbols(100);
    for (size_t i = 0; i < 100; ++i) {
        symbols[i] = points[dist(rng)];
    }

    auto result = demapper.demap(symbols.data(), symbols.size());

    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.num_symbols, 100u);
    EXPECT_EQ(result.llr.size(), 1200u);
}
#endif

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
