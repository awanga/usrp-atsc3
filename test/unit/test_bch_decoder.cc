// test_bch_decoder.cc — Unit tests for lib/fec/bch_decoder.h
//
// Tests BCH decoding, error correction, and edge cases

#include "bch_decoder.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace fec {
namespace {

//==============================================================================
// Test Helpers
//==============================================================================

// Create a valid BCH codeword (all zeros - always valid)
std::vector<uint8_t> create_zero_codeword(size_t num_bits) {
    return std::vector<uint8_t>((num_bits + 7) / 8, 0);
}

// Inject random bit errors into a codeword
void inject_errors(std::vector<uint8_t>& codeword, size_t num_bits, size_t num_errors,
                   std::vector<size_t>& error_positions, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> dist(0, num_bits - 1);

    error_positions.clear();

    // Generate unique random positions
    while (error_positions.size() < num_errors) {
        size_t pos = dist(rng);
        if (std::find(error_positions.begin(), error_positions.end(), pos) ==
            error_positions.end()) {
            error_positions.push_back(pos);
        }
    }

    // Flip bits at error positions
    for (size_t pos : error_positions) {
        size_t byte_idx = pos / 8;
        size_t bit_idx = 7 - (pos % 8);
        codeword[byte_idx] ^= (1 << bit_idx);
    }
}

// Set a specific bit in packed array
void set_bit(std::vector<uint8_t>& bits, size_t pos, bool value) {
    size_t byte_idx = pos / 8;
    size_t bit_idx = 7 - (pos % 8);
    if (value) {
        bits[byte_idx] |= (1 << bit_idx);
    } else {
        bits[byte_idx] &= ~(1 << bit_idx);
    }
}

//==============================================================================
// Construction Tests
//==============================================================================

TEST(BchDecoderTest, DefaultConstruction) {
    BchDecoder decoder;

    EXPECT_TRUE(decoder.is_initialized());
    EXPECT_EQ(decoder.parity_bits(), 192u);  // m * t = 16 * 12
}

TEST(BchDecoderTest, CustomConfig) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    EXPECT_TRUE(decoder.is_initialized());
    EXPECT_EQ(decoder.get_config().codeword_length, 1000u);
    EXPECT_EQ(decoder.get_config().info_length, 808u);
}

TEST(BchDecoderTest, Atsc3ShortFrame) {
    BchConfig config = get_atsc3_bch_config(true);

    EXPECT_EQ(config.codeword_length, 14400u);
    EXPECT_EQ(config.info_length, 14208u);  // 14400 - 192
    EXPECT_EQ(config.t, 12u);
    EXPECT_EQ(config.m, 16u);
}

TEST(BchDecoderTest, Atsc3LongFrame) {
    BchConfig config = get_atsc3_bch_config(false);

    EXPECT_EQ(config.codeword_length, 57600u);
    EXPECT_EQ(config.info_length, 57408u);  // 57600 - 192
    EXPECT_EQ(config.t, 12u);
    EXPECT_EQ(config.m, 16u);
}

//==============================================================================
// GF Arithmetic Tests (indirect via decoding)
//==============================================================================

TEST(BchDecoderTest, ZeroCodewordDecodes) {
    // Use smaller codeword for faster testing
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);
    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.was_valid);
    EXPECT_EQ(result.errors_corrected, 0u);
}

//==============================================================================
// Error Correction Tests
//==============================================================================

TEST(BchDecoderTest, SingleErrorCorrected) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    for (size_t error_pos : {0, 100, 500, 999}) {
        std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
        set_bit(codeword, error_pos, true);  // Inject single error

        BchResult result = decoder.decode(codeword.data(), config.codeword_length);

        EXPECT_TRUE(result.success) << "Failed at error position " << error_pos;
        EXPECT_EQ(result.errors_corrected, 1u) << "Wrong count at position " << error_pos;
        EXPECT_FALSE(result.was_valid);
    }
}

TEST(BchDecoderTest, SixRandomErrorsCorrected) {
    // Test with 6 random errors (well within t=12 capability)
    BchConfig config;
    config.codeword_length = 2000;
    config.info_length = 1808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    // Test with multiple random seeds
    for (unsigned seed = 1; seed <= 10; seed++) {
        std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
        std::vector<size_t> error_positions;

        inject_errors(codeword, config.codeword_length, 6, error_positions, seed);

        BchResult result = decoder.decode(codeword.data(), config.codeword_length);

        EXPECT_TRUE(result.success) << "Failed with seed " << seed;
        EXPECT_EQ(result.errors_corrected, 6u) << "Wrong error count with seed " << seed;
        EXPECT_FALSE(result.was_valid);

        // Verify all decoded bits are zero (original codeword)
        for (size_t i = 0; i < result.decoded_bits.size(); i++) {
            EXPECT_EQ(result.decoded_bits[i], 0u)
                << "Decoded bit mismatch at byte " << i << " with seed " << seed;
        }
    }
}

TEST(BchDecoderTest, TwelveErrorsCorrected) {
    // Test at maximum capability (t=12 errors)
    BchConfig config;
    config.codeword_length = 2000;
    config.info_length = 1808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    std::vector<size_t> error_positions;

    inject_errors(codeword, config.codeword_length, 12, error_positions, 42);

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.errors_corrected, 12u);

    // Verify correction
    for (size_t i = 0; i < result.decoded_bits.size(); i++) {
        EXPECT_EQ(result.decoded_bits[i], 0u) << "Decoded bit mismatch at byte " << i;
    }
}

TEST(BchDecoderTest, ThirteenErrorsFailure) {
    // Test beyond capability (13 errors, t=12)
    BchConfig config;
    config.codeword_length = 2000;
    config.info_length = 1808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    std::vector<size_t> error_positions;

    inject_errors(codeword, config.codeword_length, 13, error_positions, 42);

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    // Should fail - too many errors
    EXPECT_FALSE(result.success);
}

TEST(BchDecoderTest, ManyErrorsFailure) {
    // Test with many errors (well beyond t)
    BchConfig config;
    config.codeword_length = 2000;
    config.info_length = 1808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    std::vector<size_t> error_positions;

    inject_errors(codeword, config.codeword_length, 50, error_positions, 42);

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_FALSE(result.success);
}

//==============================================================================
// Edge Case Tests
//==============================================================================

TEST(BchDecoderTest, NullInput) {
    BchDecoder decoder;

    BchResult result = decoder.decode(nullptr, 1000);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.decoded_bits.empty());
}

TEST(BchDecoderTest, ZeroLength) {
    BchDecoder decoder;

    std::vector<uint8_t> codeword(100, 0);
    BchResult result = decoder.decode(codeword.data(), 0);

    EXPECT_FALSE(result.success);
}

TEST(BchDecoderTest, ConsecutiveErrors) {
    // Test with consecutive error positions
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);

    // Inject 6 consecutive errors starting at position 100
    for (size_t i = 0; i < 6; i++) {
        set_bit(codeword, 100 + i, true);
    }

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.errors_corrected, 6u);
}

TEST(BchDecoderTest, ErrorsAtBoundaries) {
    // Test with errors at byte boundaries
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);

    // Inject errors at byte boundaries (positions 7, 8, 15, 16, etc.)
    std::vector<size_t> positions = {7, 8, 15, 16, 23, 24};
    for (size_t pos : positions) {
        set_bit(codeword, pos, true);
    }

    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.errors_corrected, 6u);
}

//==============================================================================
// Pre-allocated Buffer Interface Tests
//==============================================================================

TEST(BchDecoderTest, PreallocatedBufferDecode) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    std::vector<size_t> error_positions;
    inject_errors(codeword, config.codeword_length, 3, error_positions, 123);

    std::vector<uint8_t> output((config.info_length + 7) / 8, 0xFF);

    int errors = decoder.decode(codeword.data(), config.codeword_length, output.data());

    EXPECT_EQ(errors, 3);

    // Verify output is corrected to zeros
    for (size_t i = 0; i < output.size(); i++) {
        EXPECT_EQ(output[i], 0u) << "Mismatch at byte " << i;
    }
}

TEST(BchDecoderTest, PreallocatedBufferFailure) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    std::vector<size_t> error_positions;
    inject_errors(codeword, config.codeword_length, 20, error_positions, 456);

    std::vector<uint8_t> output((config.info_length + 7) / 8, 0);

    int errors = decoder.decode(codeword.data(), config.codeword_length, output.data());

    EXPECT_EQ(errors, -1);  // Failure
}

//==============================================================================
// Configuration Tests
//==============================================================================

TEST(BchDecoderTest, SetConfig) {
    BchDecoder decoder;

    BchConfig new_config;
    new_config.codeword_length = 500;
    new_config.info_length = 308;
    new_config.t = 12;
    new_config.m = 16;

    decoder.set_config(new_config);

    EXPECT_EQ(decoder.get_config().codeword_length, 500u);
    EXPECT_EQ(decoder.get_config().info_length, 308u);
}

TEST(BchDecoderTest, Reset) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    // Decode something
    std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);
    set_bit(codeword, 100, true);
    decoder.decode(codeword.data(), config.codeword_length);

    // Reset and decode again
    decoder.reset();

    codeword = create_zero_codeword(config.codeword_length);
    BchResult result = decoder.decode(codeword.data(), config.codeword_length);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.was_valid);
}

//==============================================================================
// Stress Tests
//==============================================================================

TEST(BchDecoderTest, MultipleDecodes) {
    BchConfig config;
    config.codeword_length = 1000;
    config.info_length = 808;
    config.t = 12;
    config.m = 16;

    BchDecoder decoder(config);

    // Decode multiple codewords with varying error counts
    for (size_t num_errors = 0; num_errors <= 12; num_errors++) {
        std::vector<uint8_t> codeword = create_zero_codeword(config.codeword_length);

        if (num_errors > 0) {
            std::vector<size_t> error_positions;
            inject_errors(codeword, config.codeword_length, num_errors, error_positions,
                          static_cast<unsigned>(num_errors));
        }

        BchResult result = decoder.decode(codeword.data(), config.codeword_length);

        EXPECT_TRUE(result.success) << "Failed with " << num_errors << " errors";
        EXPECT_EQ(result.errors_corrected, num_errors)
            << "Wrong count with " << num_errors << " errors";
    }
}

}  // namespace
}  // namespace fec
}  // namespace atsc3
