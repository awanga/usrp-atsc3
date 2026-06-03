#pragma once

// SIMD Types and Intrinsics Wrappers
//
// Provides portable type aliases and wrapper functions for SIMD operations.
// Supports multiple tiers: SSSE3 (128-bit) and AVX2 (256-bit).
//
// Type Naming Convention:
//   simd_f32x4  - 4 floats in 128-bit register (SSE)
//   simd_f32x8  - 8 floats in 256-bit register (AVX)
//   simd_i8x16  - 16 int8_t in 128-bit register (SSE)
//   simd_i8x32  - 32 int8_t in 256-bit register (AVX2)
//   simd_i16x8  - 8 int16_t in 128-bit register (SSE)
//   simd_i16x16 - 16 int16_t in 256-bit register (AVX2)
//   simd_i32x4  - 4 int32_t in 128-bit register (SSE)
//   simd_i32x8  - 8 int32_t in 256-bit register (AVX2)
//
// Alignment Macros:
//   ATSC3_ALIGN_16 - 16-byte alignment for SSE
//   ATSC3_ALIGN_32 - 32-byte alignment for AVX
//   ATSC3_ALIGN_64 - 64-byte alignment for cache lines / AVX-512
//
// Usage:
//   ATSC3_ALIGN_32 float buffer[256];  // AVX-aligned buffer
//   simd_f32x8 v = simd_load_f32x8(buffer);
//   v = simd_abs_f32x8(v);
//   simd_store_f32x8(buffer, v);

#include <cstddef>
#include <cstdint>

// Include SIMD intrinsics based on platform and tier
#if defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)
#include <immintrin.h>  // AVX, AVX2, FMA
#elif defined(ATSC3_SIMD_SSSE3)
#include <tmmintrin.h>  // SSSE3 (pshufb, pabs, etc.)
#include <emmintrin.h>  // SSE2 (baseline)
#endif

namespace atsc3 {
namespace simd {

//------------------------------------------------------------------------------
// Alignment Macros
//------------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define ATSC3_ALIGN_16 __attribute__((aligned(16)))
#define ATSC3_ALIGN_32 __attribute__((aligned(32)))
#define ATSC3_ALIGN_64 __attribute__((aligned(64)))
#elif defined(_MSC_VER)
#define ATSC3_ALIGN_16 __declspec(align(16))
#define ATSC3_ALIGN_32 __declspec(align(32))
#define ATSC3_ALIGN_64 __declspec(align(64))
#else
#define ATSC3_ALIGN_16
#define ATSC3_ALIGN_32
#define ATSC3_ALIGN_64
#endif

// Cache line size for prefetch and buffer alignment
constexpr size_t kCacheLineSize = 64;

//------------------------------------------------------------------------------
// SSSE3 Tier Types (128-bit vectors)
//------------------------------------------------------------------------------

#if defined(ATSC3_SIMD_SSSE3) || defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

// Type aliases for 128-bit vectors
using simd_f32x4 = __m128;
using simd_i8x16 = __m128i;
using simd_i16x8 = __m128i;
using simd_i32x4 = __m128i;

// Load/Store (aligned)
inline simd_f32x4 simd_load_f32x4(const float* ptr) {
    return _mm_load_ps(ptr);
}

inline void simd_store_f32x4(float* ptr, simd_f32x4 v) {
    _mm_store_ps(ptr, v);
}

inline simd_i8x16 simd_load_i8x16(const int8_t* ptr) {
    return _mm_load_si128(reinterpret_cast<const __m128i*>(ptr));
}

inline void simd_store_i8x16(int8_t* ptr, simd_i8x16 v) {
    _mm_store_si128(reinterpret_cast<__m128i*>(ptr), v);
}

// Load/Store (unaligned)
inline simd_f32x4 simd_loadu_f32x4(const float* ptr) {
    return _mm_loadu_ps(ptr);
}

inline void simd_storeu_f32x4(float* ptr, simd_f32x4 v) {
    _mm_storeu_ps(ptr, v);
}

inline simd_i8x16 simd_loadu_i8x16(const int8_t* ptr) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
}

inline void simd_storeu_i8x16(int8_t* ptr, simd_i8x16 v) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), v);
}

// Broadcast
inline simd_f32x4 simd_set1_f32x4(float x) {
    return _mm_set1_ps(x);
}

inline simd_i8x16 simd_set1_i8x16(int8_t x) {
    return _mm_set1_epi8(x);
}

inline simd_i16x8 simd_set1_i16x8(int16_t x) {
    return _mm_set1_epi16(x);
}

inline simd_i32x4 simd_set1_i32x4(int32_t x) {
    return _mm_set1_epi32(x);
}

// Zero
inline simd_f32x4 simd_zero_f32x4() {
    return _mm_setzero_ps();
}

inline simd_i8x16 simd_zero_i8x16() {
    return _mm_setzero_si128();
}

// Arithmetic (float)
inline simd_f32x4 simd_add_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_add_ps(a, b);
}

inline simd_f32x4 simd_sub_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_sub_ps(a, b);
}

inline simd_f32x4 simd_mul_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_mul_ps(a, b);
}

inline simd_f32x4 simd_div_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_div_ps(a, b);
}

// Abs (float): clear sign bit
inline simd_f32x4 simd_abs_f32x4(simd_f32x4 x) {
    // Create mask with sign bit cleared: 0x7FFFFFFF
    __m128i mask = _mm_set1_epi32(0x7FFFFFFF);
    return _mm_and_ps(x, _mm_castsi128_ps(mask));
}

// Min/Max (float)
inline simd_f32x4 simd_min_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_min_ps(a, b);
}

inline simd_f32x4 simd_max_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_max_ps(a, b);
}

// Arithmetic (int8)
inline simd_i8x16 simd_add_i8x16(simd_i8x16 a, simd_i8x16 b) {
    return _mm_add_epi8(a, b);
}

inline simd_i8x16 simd_sub_i8x16(simd_i8x16 a, simd_i8x16 b) {
    return _mm_sub_epi8(a, b);
}

// Abs (int8): SSSE3 pabsb
inline simd_i8x16 simd_abs_i8x16(simd_i8x16 x) {
    return _mm_abs_epi8(x);
}

// Min/Max (int8): SSSE3-compatible using comparison + blendv emulation
// Note: For better performance on SSE4.1+ hosts, use _mm_min_epi8/_mm_max_epi8
inline simd_i8x16 simd_min_i8x16(simd_i8x16 a, simd_i8x16 b) {
    // min(a,b) = (a < b) ? a : b
    // Use signed comparison: a - b underflows if a < b (MSB set)
    __m128i diff = _mm_sub_epi8(a, b);
    // Create mask where a < b (signed)
    __m128i mask = _mm_cmpgt_epi8(_mm_setzero_si128(), diff);
    // Select a where mask is set, b otherwise
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

inline simd_i8x16 simd_max_i8x16(simd_i8x16 a, simd_i8x16 b) {
    // max(a,b) = (a > b) ? a : b
    __m128i diff = _mm_sub_epi8(a, b);
    __m128i mask = _mm_cmpgt_epi8(diff, _mm_setzero_si128());
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

// Shuffle (int8): SSSE3 pshufb
// idx[i] = 0x80 means output byte is zero
// idx[i] = 0-15 means output byte comes from that position in a
inline simd_i8x16 simd_shuffle_i8x16(simd_i8x16 a, simd_i8x16 idx) {
    return _mm_shuffle_epi8(a, idx);
}

// Horizontal sum (float) - sum all 4 elements
inline float simd_hsum_f32x4(simd_f32x4 v) {
    // SSE3 version using hadd
    __m128 shuf = _mm_movehdup_ps(v);       // [v1, v1, v3, v3]
    __m128 sums = _mm_add_ps(v, shuf);      // [v0+v1, v1+v1, v2+v3, v3+v3]
    shuf = _mm_movehl_ps(shuf, sums);       // [v2+v3, v3+v3, ...]
    sums = _mm_add_ss(sums, shuf);          // [v0+v1+v2+v3, ...]
    return _mm_cvtss_f32(sums);
}

// Sign extraction (float) - extract sign bits as 4-bit mask
inline int simd_signmask_f32x4(simd_f32x4 v) {
    return _mm_movemask_ps(v);
}

// Comparison
inline simd_f32x4 simd_cmplt_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_cmplt_ps(a, b);
}

inline simd_f32x4 simd_cmpgt_f32x4(simd_f32x4 a, simd_f32x4 b) {
    return _mm_cmpgt_ps(a, b);
}

// Blend (float): SSSE3-compatible using AND/OR
// Select from a (mask=0) or b (mask MSB=1)
inline simd_f32x4 simd_blend_f32x4(simd_f32x4 a, simd_f32x4 b, simd_f32x4 mask) {
    // Use sign bit as selector (consistent with SSE4.1 blendv semantics)
    // result = (a & ~mask_sign) | (b & mask_sign)
    // Create mask from sign bits: shift right arithmetic to broadcast sign
    __m128i mask_int = _mm_castps_si128(mask);
    __m128i sign_mask = _mm_srai_epi32(mask_int, 31);  // Broadcast sign bit
    __m128 mask_ps = _mm_castsi128_ps(sign_mask);
    return _mm_or_ps(_mm_andnot_ps(mask_ps, a), _mm_and_ps(mask_ps, b));
}

#endif  // ATSC3_SIMD_SSSE3

//------------------------------------------------------------------------------
// AVX2 Tier Types (256-bit vectors)
//------------------------------------------------------------------------------

#if defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

// Type aliases for 256-bit vectors
using simd_f32x8 = __m256;
using simd_i8x32 = __m256i;
using simd_i16x16 = __m256i;
using simd_i32x8 = __m256i;

// Load/Store (aligned)
inline simd_f32x8 simd_load_f32x8(const float* ptr) {
    return _mm256_load_ps(ptr);
}

inline void simd_store_f32x8(float* ptr, simd_f32x8 v) {
    _mm256_store_ps(ptr, v);
}

inline simd_i8x32 simd_load_i8x32(const int8_t* ptr) {
    return _mm256_load_si256(reinterpret_cast<const __m256i*>(ptr));
}

inline void simd_store_i8x32(int8_t* ptr, simd_i8x32 v) {
    _mm256_store_si256(reinterpret_cast<__m256i*>(ptr), v);
}

// Load/Store (unaligned)
inline simd_f32x8 simd_loadu_f32x8(const float* ptr) {
    return _mm256_loadu_ps(ptr);
}

inline void simd_storeu_f32x8(float* ptr, simd_f32x8 v) {
    _mm256_storeu_ps(ptr, v);
}

inline simd_i8x32 simd_loadu_i8x32(const int8_t* ptr) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
}

inline void simd_storeu_i8x32(int8_t* ptr, simd_i8x32 v) {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr), v);
}

// Broadcast
inline simd_f32x8 simd_set1_f32x8(float x) {
    return _mm256_set1_ps(x);
}

inline simd_i8x32 simd_set1_i8x32(int8_t x) {
    return _mm256_set1_epi8(x);
}

inline simd_i16x16 simd_set1_i16x16(int16_t x) {
    return _mm256_set1_epi16(x);
}

inline simd_i32x8 simd_set1_i32x8(int32_t x) {
    return _mm256_set1_epi32(x);
}

// Zero
inline simd_f32x8 simd_zero_f32x8() {
    return _mm256_setzero_ps();
}

inline simd_i8x32 simd_zero_i8x32() {
    return _mm256_setzero_si256();
}

// Arithmetic (float)
inline simd_f32x8 simd_add_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_add_ps(a, b);
}

inline simd_f32x8 simd_sub_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_sub_ps(a, b);
}

inline simd_f32x8 simd_mul_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_mul_ps(a, b);
}

inline simd_f32x8 simd_div_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_div_ps(a, b);
}

// FMA (fused multiply-add): a * b + c
inline simd_f32x8 simd_fmadd_f32x8(simd_f32x8 a, simd_f32x8 b, simd_f32x8 c) {
    return _mm256_fmadd_ps(a, b, c);
}

// Abs (float): clear sign bit
inline simd_f32x8 simd_abs_f32x8(simd_f32x8 x) {
    __m256i mask = _mm256_set1_epi32(0x7FFFFFFF);
    return _mm256_and_ps(x, _mm256_castsi256_ps(mask));
}

// Min/Max (float)
inline simd_f32x8 simd_min_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_min_ps(a, b);
}

inline simd_f32x8 simd_max_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_max_ps(a, b);
}

// Arithmetic (int8)
inline simd_i8x32 simd_add_i8x32(simd_i8x32 a, simd_i8x32 b) {
    return _mm256_add_epi8(a, b);
}

inline simd_i8x32 simd_sub_i8x32(simd_i8x32 a, simd_i8x32 b) {
    return _mm256_sub_epi8(a, b);
}

// Abs (int8)
inline simd_i8x32 simd_abs_i8x32(simd_i8x32 x) {
    return _mm256_abs_epi8(x);
}

// Min/Max (int8)
inline simd_i8x32 simd_min_i8x32(simd_i8x32 a, simd_i8x32 b) {
    return _mm256_min_epi8(a, b);
}

inline simd_i8x32 simd_max_i8x32(simd_i8x32 a, simd_i8x32 b) {
    return _mm256_max_epi8(a, b);
}

// Shuffle (int8): AVX2 vpshufb (shuffles within each 128-bit lane)
inline simd_i8x32 simd_shuffle_i8x32(simd_i8x32 a, simd_i8x32 idx) {
    return _mm256_shuffle_epi8(a, idx);
}

// Horizontal sum (float) - sum all 8 elements
inline float simd_hsum_f32x8(simd_f32x8 v) {
    // Sum upper and lower 128-bit halves
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    return simd_hsum_f32x4(sum);
}

// Sign extraction (float) - extract sign bits as 8-bit mask
inline int simd_signmask_f32x8(simd_f32x8 v) {
    return _mm256_movemask_ps(v);
}

// Comparison
inline simd_f32x8 simd_cmplt_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_cmp_ps(a, b, _CMP_LT_OQ);
}

inline simd_f32x8 simd_cmpgt_f32x8(simd_f32x8 a, simd_f32x8 b) {
    return _mm256_cmp_ps(a, b, _CMP_GT_OQ);
}

// Blend (float): select from a (mask=0) or b (mask=1)
inline simd_f32x8 simd_blend_f32x8(simd_f32x8 a, simd_f32x8 b, simd_f32x8 mask) {
    return _mm256_blendv_ps(a, b, mask);
}

// Gather (int32 indices, float data)
// Load floats from base[index[0]], base[index[1]], ..., base[index[7]]
inline simd_f32x8 simd_gather_f32x8(const float* base, simd_i32x8 index) {
    return _mm256_i32gather_ps(base, index, 4);  // scale=4 for float (4 bytes)
}

// Permute across 256-bit register (float)
inline simd_f32x8 simd_permute_f32x8(simd_f32x8 v, simd_i32x8 idx) {
    return _mm256_permutevar8x32_ps(v, idx);
}

#endif  // ATSC3_SIMD_AVX2

//------------------------------------------------------------------------------
// Prefetch Hints
//------------------------------------------------------------------------------

// Prefetch data into L1 cache for read
inline void simd_prefetch_read(const void* ptr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 0, 3);  // read, high locality
#elif defined(_MSC_VER)
    _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0);
#endif
}

// Prefetch data into L2 cache for read
inline void simd_prefetch_read_l2(const void* ptr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 0, 2);  // read, medium locality
#elif defined(_MSC_VER)
    _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T1);
#endif
}

// Prefetch data for write
inline void simd_prefetch_write(const void* ptr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 1, 3);  // write, high locality
#elif defined(_MSC_VER)
    _mm_prefetch(static_cast<const char*>(ptr), _MM_HINT_T0);
#endif
}

}  // namespace simd
}  // namespace atsc3
