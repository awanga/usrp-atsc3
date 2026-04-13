// test_types.cc — Unit tests for lib/types.h
//
// Verifies ATSC3_SAMPLE_T compiles correctly in both float and fixed-point
// modes, and validates size/alignment expectations.

#include <gtest/gtest.h>

#include "types.h"

namespace atsc3 {
namespace {

// Test that sample_t is correctly defined
TEST(TypesTest, SampleTypeIsDefined) {
    sample_t s{};
    (void)s;  // Suppress unused variable warning
    SUCCEED();
}

// Test sample_t size expectations
TEST(TypesTest, SampleTypeSize) {
#ifdef ATSC3_FIXED_POINT
    // Fixed-point: complex<int16_t> = 2 * 2 = 4 bytes
    EXPECT_EQ(sizeof(sample_t), 4u);
    EXPECT_EQ(sizeof(real_t), 2u);
#else
    // Float: complex<float> = 2 * 4 = 8 bytes
    EXPECT_EQ(sizeof(sample_t), 8u);
    EXPECT_EQ(sizeof(real_t), 4u);
#endif
}

// Test sample_t alignment
TEST(TypesTest, SampleTypeAlignment) {
#ifdef ATSC3_FIXED_POINT
    EXPECT_GE(alignof(sample_t), alignof(int16_t));
#else
    EXPECT_GE(alignof(sample_t), alignof(float));
#endif
}

// Test ATSC3_SAMPLE_T alias
TEST(TypesTest, SampleTypeAlias) {
    static_assert(std::is_same_v<sample_t, ATSC3_SAMPLE_T>,
                  "ATSC3_SAMPLE_T must be an alias for sample_t");
    SUCCEED();
}

// Test real_t alias
TEST(TypesTest, RealTypeAlias) {
    static_assert(std::is_same_v<real_t, ATSC3_REAL_T>,
                  "ATSC3_REAL_T must be an alias for real_t");
    SUCCEED();
}

// Test complex sample construction
TEST(TypesTest, SampleConstruction) {
#ifdef ATSC3_FIXED_POINT
    sample_t s{1000, -500};
    EXPECT_EQ(s.real(), 1000);
    EXPECT_EQ(s.imag(), -500);
#else
    sample_t s{0.5f, -0.25f};
    EXPECT_FLOAT_EQ(s.real(), 0.5f);
    EXPECT_FLOAT_EQ(s.imag(), -0.25f);
#endif
}

// Test sample arithmetic
TEST(TypesTest, SampleArithmetic) {
#ifdef ATSC3_FIXED_POINT
    sample_t a{100, 200};
    sample_t b{50, 100};
    sample_t c = a + b;
    EXPECT_EQ(c.real(), 150);
    EXPECT_EQ(c.imag(), 300);
#else
    sample_t a{1.0f, 2.0f};
    sample_t b{0.5f, 1.0f};
    sample_t c = a + b;
    EXPECT_FLOAT_EQ(c.real(), 1.5f);
    EXPECT_FLOAT_EQ(c.imag(), 3.0f);
#endif
}

#ifdef ATSC3_FIXED_POINT
// Fixed-point specific tests

TEST(TypesTest, Q15Constants) {
    EXPECT_EQ(ATSC3_Q_FRACTIONAL_BITS, 15);
    EXPECT_EQ(ATSC3_Q_SCALE, 32768);
    EXPECT_FLOAT_EQ(ATSC3_Q_SCALE_INV, 1.0f / 32768.0f);
}

TEST(TypesTest, FloatToQ15Conversion) {
    // Test conversion of common values
    EXPECT_EQ(float_to_q15(0.0f), 0);
    EXPECT_EQ(float_to_q15(0.5f), 16384);
    EXPECT_EQ(float_to_q15(-0.5f), -16384);

    // Test near-unity values (Q1.15 range is [-1, 1))
    EXPECT_EQ(float_to_q15(0.999969482421875f), 32767);  // Max positive
}

TEST(TypesTest, Q15ToFloatConversion) {
    EXPECT_FLOAT_EQ(q15_to_float(0), 0.0f);
    EXPECT_FLOAT_EQ(q15_to_float(16384), 0.5f);
    EXPECT_FLOAT_EQ(q15_to_float(-16384), -0.5f);
    EXPECT_FLOAT_EQ(q15_to_float(32767), 0.999969482421875f);
}

TEST(TypesTest, Q15RoundTrip) {
    // Test that conversion round-trips correctly for representable values
    for (int16_t i = -32768; i < 32767; i += 1000) {
        float f = q15_to_float(i);
        int16_t back = float_to_q15(f);
        EXPECT_EQ(back, i) << "Round-trip failed for " << i;
    }
}

#endif  // ATSC3_FIXED_POINT

// Test FFT size constants
TEST(ConstantsTest, FftSizes) {
    EXPECT_EQ(constants::FFT_SIZE_8K, 8192u);
    EXPECT_EQ(constants::FFT_SIZE_16K, 16384u);
    EXPECT_EQ(constants::FFT_SIZE_32K, 32768u);
    EXPECT_EQ(constants::BOOTSTRAP_FFT_SIZE, 4096u);
}

// Test LDPC codeword lengths
TEST(ConstantsTest, LdpcCodewordLengths) {
    EXPECT_EQ(constants::LDPC_SHORT_CODEWORD, 16200u);
    EXPECT_EQ(constants::LDPC_LONG_CODEWORD, 64800u);
}

// Test cyclic prefix fractions
TEST(ConstantsTest, CyclicPrefixFractions) {
    // Verify some key CP fractions
    EXPECT_EQ(constants::CP_FRACTION_512_8192, 512u);   // 1/16
    EXPECT_EQ(constants::CP_FRACTION_1024_8192, 1024u); // 1/8
    EXPECT_EQ(constants::CP_FRACTION_2048_8192, 2048u); // 1/4
    EXPECT_EQ(constants::CP_FRACTION_4096_8192, 4096u); // 1/2
}

// Test that CP fractions are in ascending order
TEST(ConstantsTest, CyclicPrefixFractionsOrdering) {
    EXPECT_LT(constants::CP_FRACTION_192_8192, constants::CP_FRACTION_384_8192);
    EXPECT_LT(constants::CP_FRACTION_384_8192, constants::CP_FRACTION_512_8192);
    EXPECT_LT(constants::CP_FRACTION_512_8192, constants::CP_FRACTION_768_8192);
    EXPECT_LT(constants::CP_FRACTION_768_8192, constants::CP_FRACTION_1024_8192);
    EXPECT_LT(constants::CP_FRACTION_1024_8192, constants::CP_FRACTION_1536_8192);
    EXPECT_LT(constants::CP_FRACTION_1536_8192, constants::CP_FRACTION_2048_8192);
    EXPECT_LT(constants::CP_FRACTION_2048_8192, constants::CP_FRACTION_2432_8192);
    EXPECT_LT(constants::CP_FRACTION_2432_8192, constants::CP_FRACTION_3072_8192);
    EXPECT_LT(constants::CP_FRACTION_3072_8192, constants::CP_FRACTION_3648_8192);
    EXPECT_LT(constants::CP_FRACTION_3648_8192, constants::CP_FRACTION_4096_8192);
}

}  // namespace
}  // namespace atsc3
