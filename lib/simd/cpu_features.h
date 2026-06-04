#pragma once

// CPU Feature Detection — Runtime SIMD capability detection
//
// Detects x86_64 SIMD instruction set support at runtime using CPUID.
// Supports SSE2, SSSE3, SSE4.1, SSE4.2, AVX, AVX2, and AVX-512 detection.
//
// Usage:
//   if (CpuFeatures::has_avx2()) {
//       // Use AVX2 code path
//   } else if (CpuFeatures::has_ssse3()) {
//       // Use SSSE3 code path
//   } else {
//       // Scalar fallback
//   }
//
// SIMD Dispatch Tiers:
//   SCALAR - No SIMD, portable C++ only
//   SSSE3  - 128-bit vectors (SSE2 + SSSE3 + SSE4.1 + SSE4.2)
//   AVX2   - 256-bit vectors (all SSE + AVX + AVX2)
//
// Note: This module is designed for x86_64 only. ARM NEON support
// would require additional implementation.

#include <cstdint>

namespace atsc3 {
namespace simd {

// SIMD dispatch tier
// Higher tiers include all capabilities of lower tiers
enum class SimdTier {
    SCALAR = 0,  // No SIMD, scalar operations only
    SSSE3 = 1,   // SSE2 + SSSE3 + SSE4.1 + SSE4.2 (128-bit vectors)
    AVX2 = 2,    // All SSE + AVX + AVX2 (256-bit vectors)
    AVX512 = 3   // All above + AVX-512F (512-bit vectors, future)
};

// Convert tier to string for logging
const char* simd_tier_name(SimdTier tier);

// CPU feature detection singleton
// Thread-safe: features detected once at first access
class CpuFeatures {
public:
    // Get singleton instance
    static const CpuFeatures& instance();

    // Individual feature queries
    bool has_sse2() const {
        return sse2_;
    }
    bool has_sse3() const {
        return sse3_;
    }
    bool has_ssse3() const {
        return ssse3_;
    }
    bool has_sse41() const {
        return sse41_;
    }
    bool has_sse42() const {
        return sse42_;
    }
    bool has_avx() const {
        return avx_;
    }
    bool has_avx2() const {
        return avx2_;
    }
    bool has_fma() const {
        return fma_;
    }
    bool has_avx512f() const {
        return avx512f_;
    }
    bool has_avx512bw() const {
        return avx512bw_;
    }
    bool has_avx512vl() const {
        return avx512vl_;
    }

    // Get the highest supported SIMD tier
    SimdTier best_tier() const {
        return best_tier_;
    }

    // Static convenience methods (call instance() internally)
    static bool has_ssse3_support() {
        return instance().has_ssse3();
    }
    static bool has_avx2_support() {
        return instance().has_avx2();
    }
    static SimdTier get_best_tier() {
        return instance().best_tier();
    }

    // Print detected features to stdout (for debugging)
    void print_features() const;

private:
    CpuFeatures();
    ~CpuFeatures() = default;

    // Disable copy/move
    CpuFeatures(const CpuFeatures&) = delete;
    CpuFeatures& operator=(const CpuFeatures&) = delete;

    // Detect CPU features via CPUID
    void detect_features();

    // Check if OS supports AVX state (XSAVE/XRSTOR)
    bool os_supports_avx() const;

    // Check if OS supports AVX-512 state
    bool os_supports_avx512() const;

    // Feature flags
    bool sse2_ = false;
    bool sse3_ = false;
    bool ssse3_ = false;
    bool sse41_ = false;
    bool sse42_ = false;
    bool avx_ = false;
    bool avx2_ = false;
    bool fma_ = false;
    bool avx512f_ = false;
    bool avx512bw_ = false;
    bool avx512vl_ = false;

    // Best supported tier
    SimdTier best_tier_ = SimdTier::SCALAR;
};

// Compile-time SIMD tier based on CMake configuration
// Set by CMake -DATSC3_SIMD_TIER=XXX
#if defined(ATSC3_SIMD_AVX2)
constexpr SimdTier kCompileTimeTier = SimdTier::AVX2;
#elif defined(ATSC3_SIMD_SSSE3)
constexpr SimdTier kCompileTimeTier = SimdTier::SSSE3;
#else
constexpr SimdTier kCompileTimeTier = SimdTier::SCALAR;
#endif

// Effective SIMD tier at runtime
// Returns minimum of compile-time tier and runtime-detected tier
inline SimdTier effective_simd_tier() {
    SimdTier runtime = CpuFeatures::get_best_tier();
    return (runtime < kCompileTimeTier) ? runtime : kCompileTimeTier;
}

}  // namespace simd
}  // namespace atsc3
