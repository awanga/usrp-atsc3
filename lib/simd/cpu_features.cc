// cpu_features.cc — Runtime CPU feature detection
//
// Uses CPUID instruction on x86_64 to detect SIMD capabilities.

#include "cpu_features.h"

#include <cstdio>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ATSC3_X86 1
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#define ATSC3_HAS_CPUID 1
#elif defined(_MSC_VER)
#include <intrin.h>
#define ATSC3_HAS_CPUID 1
#endif
#endif

namespace atsc3 {
namespace simd {

const char* simd_tier_name(SimdTier tier) {
    switch (tier) {
        case SimdTier::SCALAR:
            return "SCALAR";
        case SimdTier::SSSE3:
            return "SSSE3";
        case SimdTier::AVX2:
            return "AVX2";
        case SimdTier::AVX512:
            return "AVX512";
        default:
            return "UNKNOWN";
    }
}

const CpuFeatures& CpuFeatures::instance() {
    static CpuFeatures features;
    return features;
}

CpuFeatures::CpuFeatures() {
    detect_features();
}

void CpuFeatures::detect_features() {
#if defined(ATSC3_HAS_CPUID)
    // CPUID function 0: Get highest supported function and vendor string
    unsigned int eax, ebx, ecx, edx;

#if defined(__GNUC__) || defined(__clang__)
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        // CPUID not supported
        return;
    }
#elif defined(_MSC_VER)
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    eax = cpuInfo[0];
    ebx = cpuInfo[1];
    ecx = cpuInfo[2];
    edx = cpuInfo[3];
#endif

    unsigned int max_func = eax;
    if (max_func < 1) {
        return;
    }

    // CPUID function 1: Processor features
#if defined(__GNUC__) || defined(__clang__)
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
#elif defined(_MSC_VER)
    __cpuid(cpuInfo, 1);
    eax = cpuInfo[0];
    ebx = cpuInfo[1];
    ecx = cpuInfo[2];
    edx = cpuInfo[3];
#endif

    // EDX bits
    sse2_ = (edx >> 26) & 1;  // bit 26: SSE2

    // ECX bits
    sse3_ = (ecx >> 0) & 1;          // bit 0: SSE3
    ssse3_ = (ecx >> 9) & 1;         // bit 9: SSSE3
    sse41_ = (ecx >> 19) & 1;        // bit 19: SSE4.1
    sse42_ = (ecx >> 20) & 1;        // bit 20: SSE4.2
    fma_ = (ecx >> 12) & 1;          // bit 12: FMA
    bool osxsave = (ecx >> 27) & 1;  // bit 27: OSXSAVE (OS uses XSAVE)
    bool avx_hw = (ecx >> 28) & 1;   // bit 28: AVX hardware support

    // AVX requires both hardware support and OS support (for saving YMM state)
    if (osxsave && avx_hw && os_supports_avx()) {
        avx_ = true;

        // Check extended features (function 7)
        if (max_func >= 7) {
#if defined(__GNUC__) || defined(__clang__)
            __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
#elif defined(_MSC_VER)
            __cpuidex(cpuInfo, 7, 0);
            eax = cpuInfo[0];
            ebx = cpuInfo[1];
            ecx = cpuInfo[2];
            edx = cpuInfo[3];
#endif

            // EBX bits
            avx2_ = (ebx >> 5) & 1;  // bit 5: AVX2

            // AVX-512 (future - requires additional OS support check)
            bool avx512f_hw = (ebx >> 16) & 1;   // bit 16: AVX-512F
            bool avx512bw_hw = (ebx >> 30) & 1;  // bit 30: AVX-512BW
            bool avx512vl_hw = (ebx >> 31) & 1;  // bit 31: AVX-512VL

            if (avx512f_hw && os_supports_avx512()) {
                avx512f_ = true;
                avx512bw_ = avx512bw_hw;
                avx512vl_ = avx512vl_hw;
            }
        }
    }
#endif  // ATSC3_HAS_CPUID

    // Determine best tier
    if (avx512f_ && avx512bw_ && avx512vl_) {
        best_tier_ = SimdTier::AVX512;
    } else if (avx2_) {
        best_tier_ = SimdTier::AVX2;
    } else if (ssse3_ && sse41_ && sse42_) {
        best_tier_ = SimdTier::SSSE3;
    } else {
        best_tier_ = SimdTier::SCALAR;
    }
}

bool CpuFeatures::os_supports_avx() const {
#if defined(ATSC3_X86)
    // Check if XCR0[2:1] = '11b' (OS has enabled XMM and YMM state)
    // Use XGETBV with ECX=0 to read XCR0
#if defined(__GNUC__) || defined(__clang__)
    unsigned int xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    return (xcr0_lo & 0x06) == 0x06;  // bits 1 and 2 set
#elif defined(_MSC_VER)
    unsigned long long xcr0 = _xgetbv(0);
    return (xcr0 & 0x06) == 0x06;
#else
    return false;
#endif
#else
    return false;
#endif
}

bool CpuFeatures::os_supports_avx512() const {
#if defined(ATSC3_X86)
    // Check if XCR0[7:5] = '111b' (OS supports opmask, ZMM_Hi256, Hi16_ZMM)
    // plus bits 1 and 2 for XMM and YMM
#if defined(__GNUC__) || defined(__clang__)
    unsigned int xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    return (xcr0_lo & 0xE6) == 0xE6;  // bits 1, 2, 5, 6, 7 set
#elif defined(_MSC_VER)
    unsigned long long xcr0 = _xgetbv(0);
    return (xcr0 & 0xE6) == 0xE6;
#else
    return false;
#endif
#else
    return false;
#endif
}

void CpuFeatures::print_features() const {
    std::printf("CPU SIMD Features:\n");
    std::printf("  SSE2:      %s\n", sse2_ ? "yes" : "no");
    std::printf("  SSE3:      %s\n", sse3_ ? "yes" : "no");
    std::printf("  SSSE3:     %s\n", ssse3_ ? "yes" : "no");
    std::printf("  SSE4.1:    %s\n", sse41_ ? "yes" : "no");
    std::printf("  SSE4.2:    %s\n", sse42_ ? "yes" : "no");
    std::printf("  AVX:       %s\n", avx_ ? "yes" : "no");
    std::printf("  AVX2:      %s\n", avx2_ ? "yes" : "no");
    std::printf("  FMA:       %s\n", fma_ ? "yes" : "no");
    std::printf("  AVX-512F:  %s\n", avx512f_ ? "yes" : "no");
    std::printf("  AVX-512BW: %s\n", avx512bw_ ? "yes" : "no");
    std::printf("  AVX-512VL: %s\n", avx512vl_ ? "yes" : "no");
    std::printf("  Best Tier: %s\n", simd_tier_name(best_tier_));
}

}  // namespace simd
}  // namespace atsc3
