// test_simd.cc — SIMD infrastructure unit tests
//
// Tests CPU feature detection and SIMD type operations.
// Verifies SIMD and scalar implementations produce equivalent results.

#include <gtest/gtest.h>

#include "simd/cpu_features.h"
#include "simd/simd_types.h"

#include <cmath>
#include <random>
#include <vector>

using namespace atsc3::simd;

//------------------------------------------------------------------------------
// CPU Feature Detection Tests
//------------------------------------------------------------------------------

TEST(CpuFeaturesTest, InstanceIsValid) {
    const CpuFeatures& features = CpuFeatures::instance();
    // Should always have at least SSE2 on x86_64
#if defined(__x86_64__) || defined(_M_X64)
    EXPECT_TRUE(features.has_sse2());
#endif
}

TEST(CpuFeaturesTest, BestTierIsValid) {
    SimdTier tier = CpuFeatures::get_best_tier();
    EXPECT_GE(static_cast<int>(tier), static_cast<int>(SimdTier::SCALAR));
    EXPECT_LE(static_cast<int>(tier), static_cast<int>(SimdTier::AVX512));
}

TEST(CpuFeaturesTest, TierNameReturnsString) {
    EXPECT_STREQ(simd_tier_name(SimdTier::SCALAR), "SCALAR");
    EXPECT_STREQ(simd_tier_name(SimdTier::SSSE3), "SSSE3");
    EXPECT_STREQ(simd_tier_name(SimdTier::AVX2), "AVX2");
    EXPECT_STREQ(simd_tier_name(SimdTier::AVX512), "AVX512");
}

TEST(CpuFeaturesTest, TierHierarchy) {
    const CpuFeatures& f = CpuFeatures::instance();

    // If AVX2 is available, SSSE3 must also be available
    if (f.has_avx2()) {
        EXPECT_TRUE(f.has_avx());
        EXPECT_TRUE(f.has_ssse3());
        EXPECT_TRUE(f.has_sse42());
    }

    // If AVX is available, SSE4.2 must be available
    if (f.has_avx()) {
        EXPECT_TRUE(f.has_sse42());
        EXPECT_TRUE(f.has_sse41());
    }

    // If SSSE3 is available, SSE3 must be available
    if (f.has_ssse3()) {
        EXPECT_TRUE(f.has_sse3());
        EXPECT_TRUE(f.has_sse2());
    }
}

TEST(CpuFeaturesTest, EffectiveTierNotHigherThanCompileTime) {
    SimdTier effective = effective_simd_tier();
    EXPECT_LE(static_cast<int>(effective), static_cast<int>(kCompileTimeTier));
}

//------------------------------------------------------------------------------
// SIMD Type Tests (SSSE3)
//------------------------------------------------------------------------------

#if defined(ATSC3_SIMD_SSSE3) || defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

TEST(SimdTypesTest, Float4LoadStore) {
    ATSC3_ALIGN_16 float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ATSC3_ALIGN_16 float result[4];

    simd_f32x4 v = simd_load_f32x4(data);
    simd_store_f32x4(result, v);

    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(result[i], data[i]);
    }
}

TEST(SimdTypesTest, Float4Arithmetic) {
    ATSC3_ALIGN_16 float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ATSC3_ALIGN_16 float b[4] = {4.0f, 3.0f, 2.0f, 1.0f};
    ATSC3_ALIGN_16 float result[4];

    simd_f32x4 va = simd_load_f32x4(a);
    simd_f32x4 vb = simd_load_f32x4(b);

    // Add
    simd_store_f32x4(result, simd_add_f32x4(va, vb));
    EXPECT_FLOAT_EQ(result[0], 5.0f);
    EXPECT_FLOAT_EQ(result[1], 5.0f);
    EXPECT_FLOAT_EQ(result[2], 5.0f);
    EXPECT_FLOAT_EQ(result[3], 5.0f);

    // Multiply
    simd_store_f32x4(result, simd_mul_f32x4(va, vb));
    EXPECT_FLOAT_EQ(result[0], 4.0f);
    EXPECT_FLOAT_EQ(result[1], 6.0f);
    EXPECT_FLOAT_EQ(result[2], 6.0f);
    EXPECT_FLOAT_EQ(result[3], 4.0f);
}

TEST(SimdTypesTest, Float4Abs) {
    ATSC3_ALIGN_16 float data[4] = {-1.0f, 2.0f, -3.0f, 4.0f};
    ATSC3_ALIGN_16 float result[4];

    simd_f32x4 v = simd_load_f32x4(data);
    simd_store_f32x4(result, simd_abs_f32x4(v));

    EXPECT_FLOAT_EQ(result[0], 1.0f);
    EXPECT_FLOAT_EQ(result[1], 2.0f);
    EXPECT_FLOAT_EQ(result[2], 3.0f);
    EXPECT_FLOAT_EQ(result[3], 4.0f);
}

TEST(SimdTypesTest, Float4MinMax) {
    ATSC3_ALIGN_16 float a[4] = {1.0f, 5.0f, 3.0f, 7.0f};
    ATSC3_ALIGN_16 float b[4] = {2.0f, 4.0f, 6.0f, 8.0f};
    ATSC3_ALIGN_16 float result[4];

    simd_f32x4 va = simd_load_f32x4(a);
    simd_f32x4 vb = simd_load_f32x4(b);

    // Min
    simd_store_f32x4(result, simd_min_f32x4(va, vb));
    EXPECT_FLOAT_EQ(result[0], 1.0f);
    EXPECT_FLOAT_EQ(result[1], 4.0f);
    EXPECT_FLOAT_EQ(result[2], 3.0f);
    EXPECT_FLOAT_EQ(result[3], 7.0f);

    // Max
    simd_store_f32x4(result, simd_max_f32x4(va, vb));
    EXPECT_FLOAT_EQ(result[0], 2.0f);
    EXPECT_FLOAT_EQ(result[1], 5.0f);
    EXPECT_FLOAT_EQ(result[2], 6.0f);
    EXPECT_FLOAT_EQ(result[3], 8.0f);
}

TEST(SimdTypesTest, Float4HorizontalSum) {
    ATSC3_ALIGN_16 float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    simd_f32x4 v = simd_load_f32x4(data);
    float sum = simd_hsum_f32x4(v);
    EXPECT_FLOAT_EQ(sum, 10.0f);
}

TEST(SimdTypesTest, Int8x16LoadStore) {
    ATSC3_ALIGN_16 int8_t data[16];
    ATSC3_ALIGN_16 int8_t result[16];

    for (int i = 0; i < 16; ++i) {
        data[i] = static_cast<int8_t>(i - 8);
    }

    simd_i8x16 v = simd_load_i8x16(data);
    simd_store_i8x16(result, v);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(result[i], data[i]);
    }
}

TEST(SimdTypesTest, Int8x16Abs) {
    ATSC3_ALIGN_16 int8_t data[16];
    ATSC3_ALIGN_16 int8_t result[16];

    for (int i = 0; i < 16; ++i) {
        data[i] = static_cast<int8_t>(i - 8);  // -8 to 7
    }

    simd_i8x16 v = simd_load_i8x16(data);
    simd_store_i8x16(result, simd_abs_i8x16(v));

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(result[i], std::abs(data[i]));
    }
}

TEST(SimdTypesTest, Int8x16Shuffle) {
    ATSC3_ALIGN_16 int8_t data[16];
    ATSC3_ALIGN_16 int8_t idx[16];
    ATSC3_ALIGN_16 int8_t result[16];

    for (int i = 0; i < 16; ++i) {
        data[i] = static_cast<int8_t>(i);
        idx[i] = static_cast<int8_t>(15 - i);  // Reverse
    }

    simd_i8x16 vdata = simd_load_i8x16(data);
    simd_i8x16 vidx = simd_load_i8x16(idx);
    simd_store_i8x16(result, simd_shuffle_i8x16(vdata, vidx));

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(result[i], 15 - i);
    }
}

#endif  // ATSC3_SIMD_SSSE3

//------------------------------------------------------------------------------
// SIMD Type Tests (AVX2)
//------------------------------------------------------------------------------

#if defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

TEST(SimdTypesAVX2Test, Float8LoadStore) {
    if (!CpuFeatures::has_avx2_support()) {
        GTEST_SKIP() << "AVX2 not supported";
    }

    ATSC3_ALIGN_32 float data[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    ATSC3_ALIGN_32 float result[8];

    simd_f32x8 v = simd_load_f32x8(data);
    simd_store_f32x8(result, v);

    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(result[i], data[i]);
    }
}

TEST(SimdTypesAVX2Test, Float8HorizontalSum) {
    if (!CpuFeatures::has_avx2_support()) {
        GTEST_SKIP() << "AVX2 not supported";
    }

    ATSC3_ALIGN_32 float data[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    simd_f32x8 v = simd_load_f32x8(data);
    float sum = simd_hsum_f32x8(v);
    EXPECT_FLOAT_EQ(sum, 36.0f);
}

TEST(SimdTypesAVX2Test, Float8FMA) {
    if (!CpuFeatures::has_avx2_support()) {
        GTEST_SKIP() << "AVX2 not supported";
    }

    ATSC3_ALIGN_32 float a[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    ATSC3_ALIGN_32 float b[8] = {2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f};
    ATSC3_ALIGN_32 float c[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    ATSC3_ALIGN_32 float result[8];

    simd_f32x8 va = simd_load_f32x8(a);
    simd_f32x8 vb = simd_load_f32x8(b);
    simd_f32x8 vc = simd_load_f32x8(c);

    // FMA: a * b + c
    simd_store_f32x8(result, simd_fmadd_f32x8(va, vb, vc));

    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(result[i], a[i] * b[i] + c[i]);
    }
}

TEST(SimdTypesAVX2Test, Int8x32Abs) {
    if (!CpuFeatures::has_avx2_support()) {
        GTEST_SKIP() << "AVX2 not supported";
    }

    ATSC3_ALIGN_32 int8_t data[32];
    ATSC3_ALIGN_32 int8_t result[32];

    for (int i = 0; i < 32; ++i) {
        data[i] = static_cast<int8_t>(i - 16);
    }

    simd_i8x32 v = simd_load_i8x32(data);
    simd_store_i8x32(result, simd_abs_i8x32(v));

    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(result[i], std::abs(data[i]));
    }
}

#endif  // ATSC3_SIMD_AVX2

//------------------------------------------------------------------------------
// Alignment Tests
//------------------------------------------------------------------------------

TEST(SimdAlignmentTest, Align16) {
    ATSC3_ALIGN_16 float buffer[4];
    uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);
    EXPECT_EQ(addr % 16, 0u);
}

TEST(SimdAlignmentTest, Align32) {
    ATSC3_ALIGN_32 float buffer[8];
    uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);
    EXPECT_EQ(addr % 32, 0u);
}

TEST(SimdAlignmentTest, Align64) {
    ATSC3_ALIGN_64 float buffer[16];
    uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);
    EXPECT_EQ(addr % 64, 0u);
}
