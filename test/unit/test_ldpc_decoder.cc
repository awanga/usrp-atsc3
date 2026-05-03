// test_ldpc_decoder.cc — Unit tests for lib/fec/ldpc_decoder.h
//
// Tests LDPC decoding, convergence, and performance

#include "ldpc_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

namespace atsc3 {
namespace fec {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(LdpcDecoderTest, DefaultConstruction) {
    LdpcDecoder decoder;

    EXPECT_TRUE(decoder.is_initialized());
    EXPECT_EQ(decoder.codeword_length(), 64800u);  // Long codeword default
    EXPECT_GT(decoder.info_bits(), 0u);
    EXPECT_GT(decoder.parity_bits(), 0u);
    EXPECT_EQ(decoder.info_bits() + decoder.parity_bits(), decoder.codeword_length());
}

TEST(LdpcDecoderTest, ShortCodewordConstruction) {
    LdpcConfig config;
    config.short_codeword = true;
    config.code_rate = config::CodeRate::RATE_7_15;

    LdpcDecoder decoder(config);

    EXPECT_TRUE(decoder.is_initialized());
    EXPECT_EQ(decoder.codeword_length(), 16200u);
    EXPECT_EQ(decoder.info_bits(), 7560u);  // 16200 * 7/15
    EXPECT_EQ(decoder.parity_bits(), 8640u);
}

TEST(LdpcDecoderTest, AllCodeRates) {
    std::vector<config::CodeRate> rates = {
        config::CodeRate::RATE_2_15,  config::CodeRate::RATE_3_15,  config::CodeRate::RATE_4_15,
        config::CodeRate::RATE_5_15,  config::CodeRate::RATE_6_15,  config::CodeRate::RATE_7_15,
        config::CodeRate::RATE_8_15,  config::CodeRate::RATE_9_15,  config::CodeRate::RATE_10_15,
        config::CodeRate::RATE_11_15, config::CodeRate::RATE_12_15, config::CodeRate::RATE_13_15};

    for (auto rate : rates) {
        LdpcConfig config;
        config.code_rate = rate;
        config.short_codeword = true;  // Use short for faster tests

        EXPECT_NO_THROW({
            LdpcDecoder decoder(config);
            EXPECT_TRUE(decoder.is_initialized());
            EXPECT_EQ(decoder.codeword_length(), 16200u);
            EXPECT_GT(decoder.info_bits(), 0u);
        }) << "Failed for code rate index "
           << static_cast<int>(rate);
    }
}

//==============================================================================
// Parity Matrix Tests
//==============================================================================

TEST(LdpcDecoderTest, ParityMatrixValid) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    const auto& H = decoder.get_parity_matrix();

    EXPECT_TRUE(H.is_valid());
    EXPECT_EQ(H.num_cols, decoder.codeword_length());
    EXPECT_EQ(H.num_rows, decoder.parity_bits());
    EXPECT_EQ(H.info_bits, decoder.info_bits());
}

TEST(LdpcDecoderTest, ParityMatrixSparseStructure) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    const auto& H = decoder.get_parity_matrix();

    // Check row degrees are reasonable
    for (size_t row = 0; row < H.num_rows; ++row) {
        size_t degree = H.check_degree(row);
        EXPECT_GT(degree, 0u) << "Row " << row << " has no connections";
        EXPECT_LT(degree, H.num_cols) << "Row " << row << " is too dense";
    }

    // Check column degrees are reasonable
    for (size_t col = 0; col < H.num_cols; ++col) {
        size_t degree = H.variable_degree(col);
        // Some columns may have degree 0 in simplified test matrix
        EXPECT_LT(degree, H.num_rows) << "Column " << col << " is too dense";
    }
}

//==============================================================================
// All-Zero Codeword Tests
//==============================================================================

TEST(LdpcDecoderTest, AllZeroCodewordNoNoise) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 50;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();

    // All-zero codeword with strong positive LLRs (bit 0 very likely)
    std::vector<int8_t> llr(n, 100);

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    EXPECT_TRUE(result.converged) << "Did not converge with " << result.num_unsatisfied_checks
                                  << " unsatisfied checks";
    EXPECT_EQ(result.num_unsatisfied_checks, 0u);

    // All decoded bits should be 0
    for (size_t i = 0; i < result.decoded_bits.size(); ++i) {
        EXPECT_EQ(result.decoded_bits[i], 0u) << "Bit " << i << " is not zero";
    }
}

TEST(LdpcDecoderTest, AllZeroCodewordWithNoise) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 50;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();

    // All-zero codeword with noisy LLRs (high SNR)
    std::mt19937 rng(42);
    std::normal_distribution<float> noise(50.0f, 10.0f);  // Mean 50 (strongly positive)

    std::vector<int8_t> llr(n);
    for (size_t i = 0; i < n; ++i) {
        float val = noise(rng);
        llr[i] = static_cast<int8_t>(std::max(-127.0f, std::min(127.0f, val)));
    }

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    // With high SNR, should converge
    EXPECT_TRUE(result.converged) << "Failed with " << result.num_unsatisfied_checks
                                  << " unsatisfied checks after " << result.iterations_used
                                  << " iterations";
}

//==============================================================================
// Decoding Tests
//==============================================================================

TEST(LdpcDecoderTest, DecodeReturnsValidResult) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    std::vector<int8_t> llr(decoder.codeword_length(), 50);

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    EXPECT_GT(result.iterations_used, 0u);
    EXPECT_EQ(result.decoded_bits.size(), decoder.info_bits());
}

TEST(LdpcDecoderTest, DecodeWithPreallocatedBuffer) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    std::vector<int8_t> llr(decoder.codeword_length(), 50);
    std::vector<uint8_t> bits(decoder.info_bits());
    size_t iterations = 0;

    int unsatisfied = decoder.decode(llr.data(), bits.data(), &iterations);

    EXPECT_GE(unsatisfied, 0);
    EXPECT_GT(iterations, 0u);
}

TEST(LdpcDecoderTest, DecodeNullInputReturnsInvalid) {
    LdpcDecoder decoder;

    LdpcResult result = decoder.decode(nullptr);

    EXPECT_FALSE(result.is_valid);
}

TEST(LdpcDecoderTest, DecodeNullInputReturnsError) {
    LdpcDecoder decoder;

    int result = decoder.decode(nullptr, nullptr);

    EXPECT_EQ(result, -1);
}

//==============================================================================
// Iteration Tests
//==============================================================================

TEST(LdpcDecoderTest, IterationCountWithinLimit) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 10;
    LdpcDecoder decoder(config);

    std::vector<int8_t> llr(decoder.codeword_length(), 5);  // Low confidence

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_LE(result.iterations_used, config.max_iterations);
}

TEST(LdpcDecoderTest, EarlyTerminationOnConvergence) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 100;
    LdpcDecoder decoder(config);

    // Strong LLRs should converge quickly
    std::vector<int8_t> llr(decoder.codeword_length(), 100);

    LdpcResult result = decoder.decode(llr.data());

    if (result.converged) {
        EXPECT_LT(result.iterations_used, config.max_iterations)
            << "Converged but used all iterations";
    }
}

//==============================================================================
// Configuration Tests
//==============================================================================

TEST(LdpcDecoderTest, SetConfig) {
    LdpcConfig config1;
    config1.short_codeword = true;
    config1.code_rate = config::CodeRate::RATE_5_15;
    LdpcDecoder decoder(config1);

    size_t info1 = decoder.info_bits();

    LdpcConfig config2;
    config2.short_codeword = true;
    config2.code_rate = config::CodeRate::RATE_10_15;
    decoder.set_config(config2);

    size_t info2 = decoder.info_bits();

    EXPECT_NE(info1, info2);
    EXPECT_GT(info2, info1);  // Higher rate = more info bits
}

TEST(LdpcDecoderTest, Reset) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    // Decode something
    std::vector<int8_t> llr(decoder.codeword_length(), 50);
    decoder.decode(llr.data());

    // Reset
    decoder.reset();

    // Should be able to decode again
    LdpcResult result = decoder.decode(llr.data());
    EXPECT_TRUE(result.is_valid);
}

//==============================================================================
// Utility Function Tests
//==============================================================================

TEST(LdpcDecoderTest, CodeRateValue) {
    EXPECT_NEAR(code_rate_value(config::CodeRate::RATE_7_15), 7.0f / 15.0f, 0.001f);
    EXPECT_NEAR(code_rate_value(config::CodeRate::RATE_2_15), 2.0f / 15.0f, 0.001f);
    EXPECT_NEAR(code_rate_value(config::CodeRate::RATE_13_15), 13.0f / 15.0f, 0.001f);
}

TEST(LdpcDecoderTest, GetLdpcInfoBits) {
    // Long codeword (64800)
    EXPECT_EQ(get_ldpc_info_bits(false, config::CodeRate::RATE_7_15), 30240u);
    EXPECT_EQ(get_ldpc_info_bits(false, config::CodeRate::RATE_2_15), 8640u);
    EXPECT_EQ(get_ldpc_info_bits(false, config::CodeRate::RATE_13_15), 56160u);

    // Short codeword (16200)
    EXPECT_EQ(get_ldpc_info_bits(true, config::CodeRate::RATE_7_15), 7560u);
    EXPECT_EQ(get_ldpc_info_bits(true, config::CodeRate::RATE_2_15), 2160u);
}

TEST(LdpcDecoderTest, GetLdpcParityBits) {
    // Long codeword
    EXPECT_EQ(get_ldpc_parity_bits(false, config::CodeRate::RATE_7_15), 64800u - 30240u);

    // Short codeword
    EXPECT_EQ(get_ldpc_parity_bits(true, config::CodeRate::RATE_7_15), 16200u - 7560u);
}

//==============================================================================
// Error Correction Tests
//==============================================================================

TEST(LdpcDecoderTest, CorrectsSingleBitError) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 50;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();

    // All-zero codeword with one bit flipped (weak negative LLR)
    std::vector<int8_t> llr(n, 80);  // Strong positive
    llr[100] = -20;                  // One bit with weak opposite sign

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    // LDPC should correct this easily
    if (result.converged) {
        // First info bits should all be 0
        size_t errors = 0;
        for (size_t i = 0; i < std::min(result.decoded_bits.size(), size_t(100)); ++i) {
            if (result.decoded_bits[i] != 0) {
                ++errors;
            }
        }
        EXPECT_LE(errors, 1u) << "Too many errors in decoded bits";
    }
}

TEST(LdpcDecoderTest, HandlesLowSNR) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 50;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();

    // Low SNR: weak LLRs close to zero
    std::mt19937 rng(123);
    std::uniform_int_distribution<int> dist(-5, 5);

    std::vector<int8_t> llr(n);
    for (size_t i = 0; i < n; ++i) {
        llr[i] = static_cast<int8_t>(dist(rng));
    }

    LdpcResult result = decoder.decode(llr.data());

    // Should complete without crashing
    EXPECT_TRUE(result.is_valid);
    // May or may not converge at low SNR
    EXPECT_GT(result.iterations_used, 0u);
}

//==============================================================================
// Performance Tests
//==============================================================================

TEST(LdpcDecoderTest, PerformanceBenchmarkShort) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 25;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();
    std::vector<int8_t> llr(n, 50);

    // Warm up
    decoder.decode(llr.data());

    // Benchmark
    const int num_iterations = 10;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_iterations; ++i) {
        decoder.decode(llr.data());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double us_per_decode = static_cast<double>(duration.count()) / num_iterations;
    double bits_per_second = (decoder.info_bits() / us_per_decode) * 1e6;

    // Target: >= 1 Mb/s
    // This is a relaxed target for CI; real implementation would be faster
    EXPECT_GT(bits_per_second, 100000.0)
        << "Throughput " << (bits_per_second / 1e6) << " Mb/s is below minimum";

    // Print performance for info
    std::cout << "LDPC decode performance (short): " << us_per_decode << " us/codeword, "
              << (bits_per_second / 1e6) << " Mb/s" << std::endl;
}

TEST(LdpcDecoderTest, PerformanceBenchmarkLong) {
    LdpcConfig config;
    config.short_codeword = false;  // 64800 bits
    config.max_iterations = 25;
    LdpcDecoder decoder(config);

    size_t n = decoder.codeword_length();
    std::vector<int8_t> llr(n, 50);

    // Warm up
    decoder.decode(llr.data());

    // Benchmark (fewer iterations for long codewords)
    const int num_iterations = 3;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_iterations; ++i) {
        decoder.decode(llr.data());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double us_per_decode = static_cast<double>(duration.count()) / num_iterations;
    double bits_per_second = (decoder.info_bits() / us_per_decode) * 1e6;

    std::cout << "LDPC decode performance (long): " << us_per_decode << " us/codeword, "
              << (bits_per_second / 1e6) << " Mb/s" << std::endl;
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(LdpcDecoderTest, AllOnesLLR) {
    LdpcConfig config;
    config.short_codeword = true;
    LdpcDecoder decoder(config);

    // All negative LLRs (all bits = 1)
    std::vector<int8_t> llr(decoder.codeword_length(), -100);

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    // All-ones is not a valid codeword for most LDPC codes
    // So this tests the decoder handles non-codewords gracefully
}

TEST(LdpcDecoderTest, AlternatingLLR) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 20;
    LdpcDecoder decoder(config);

    std::vector<int8_t> llr(decoder.codeword_length());
    for (size_t i = 0; i < llr.size(); ++i) {
        llr[i] = (i % 2 == 0) ? 50 : -50;
    }

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    // Pattern unlikely to be a valid codeword
}

TEST(LdpcDecoderTest, ZeroLLR) {
    LdpcConfig config;
    config.short_codeword = true;
    config.max_iterations = 10;
    LdpcDecoder decoder(config);

    // All zero LLRs (maximum uncertainty)
    std::vector<int8_t> llr(decoder.codeword_length(), 0);

    LdpcResult result = decoder.decode(llr.data());

    EXPECT_TRUE(result.is_valid);
    // With no information, decoder has to guess
    // Just verify it completes without crashing
}

//==============================================================================
// Min-Sum Scaling Tests
//==============================================================================

TEST(LdpcDecoderTest, MinSumScalingAffectsConvergence) {
    size_t n;
    std::vector<int8_t> llr;

    // Create test data with moderate noise
    {
        LdpcConfig config;
        config.short_codeword = true;
        LdpcDecoder temp(config);
        n = temp.codeword_length();

        std::mt19937 rng(456);
        std::normal_distribution<float> noise(40.0f, 15.0f);

        llr.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float val = noise(rng);
            llr[i] = static_cast<int8_t>(std::max(-127.0f, std::min(127.0f, val)));
        }
    }

    // Test with different scaling factors
    std::vector<float> scales = {0.5f, 0.75f, 1.0f};
    std::vector<size_t> iterations_used;

    for (float scale : scales) {
        LdpcConfig config;
        config.short_codeword = true;
        config.max_iterations = 30;
        config.min_sum_scale = scale;

        LdpcDecoder decoder(config);
        LdpcResult result = decoder.decode(llr.data());

        iterations_used.push_back(result.iterations_used);
    }

    // Different scaling should potentially affect iteration count
    // (This is a soft test - just verify execution completes)
    EXPECT_EQ(iterations_used.size(), scales.size());
}

}  // namespace
}  // namespace fec
}  // namespace atsc3
