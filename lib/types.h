#pragma once

// gr-atsc3 Core Type Definitions
// ATSC3_SAMPLE_T typedef switching on ATSC3_FIXED_POINT

#include <complex>
#include <cstdint>

namespace atsc3 {

// Sample type for DSP processing
// Float mode (default): std::complex<float>
// Fixed-point mode: std::complex<int16_t> (Q1.15 format)
#ifdef ATSC3_FIXED_POINT
using sample_t = std::complex<int16_t>;
using real_t = int16_t;

// Q-format scaling constants for fixed-point arithmetic
// Q1.15: 1 sign bit, 0 integer bits, 15 fractional bits
// Range: [-1.0, 1.0) with resolution 2^-15 = 3.05e-5
constexpr int ATSC3_Q_FRACTIONAL_BITS = 15;
constexpr int32_t ATSC3_Q_SCALE = 1 << ATSC3_Q_FRACTIONAL_BITS;  // 32768
constexpr float ATSC3_Q_SCALE_INV = 1.0f / static_cast<float>(ATSC3_Q_SCALE);

// Convert float to Q1.15
// Saturates to [-32767, 32767] (symmetric) rather than wrapping: an
// unclamped cast is undefined behavior for any x whose scaled value
// falls outside int16_t range (e.g. float_to_q15(1.0f) scales to
// exactly 32768).
inline int16_t float_to_q15(float x) {
    float scaled = x * static_cast<float>(ATSC3_Q_SCALE);
    if (scaled > 32767.0f) {
        return 32767;
    }
    if (scaled < -32767.0f) {
        return -32767;
    }
    return static_cast<int16_t>(scaled);
}

// Convert Q1.15 to float
inline float q15_to_float(int16_t x) {
    return static_cast<float>(x) * ATSC3_Q_SCALE_INV;
}

#else
using sample_t = std::complex<float>;
using real_t = float;
#endif

// Alias for compatibility with older code
using ATSC3_SAMPLE_T = sample_t;
using ATSC3_REAL_T = real_t;

// Common ATSC 3.0 constants
namespace constants {

// FFT sizes supported by ATSC 3.0
constexpr size_t FFT_SIZE_8K = 8192;
constexpr size_t FFT_SIZE_16K = 16384;
constexpr size_t FFT_SIZE_32K = 32768;

// Bootstrap is always 4K OFDM regardless of data FFT size
constexpr size_t BOOTSTRAP_FFT_SIZE = 4096;

// Cyclic prefix fractions (as numerator over 8192)
constexpr size_t CP_FRACTION_192_8192 = 192;    // 192/8192 = 1/42.67
constexpr size_t CP_FRACTION_384_8192 = 384;    // 384/8192 = 1/21.33
constexpr size_t CP_FRACTION_512_8192 = 512;    // 512/8192 = 1/16
constexpr size_t CP_FRACTION_768_8192 = 768;    // 768/8192 = 1/10.67
constexpr size_t CP_FRACTION_1024_8192 = 1024;  // 1024/8192 = 1/8
constexpr size_t CP_FRACTION_1536_8192 = 1536;  // 1536/8192 = 1/5.33
constexpr size_t CP_FRACTION_2048_8192 = 2048;  // 2048/8192 = 1/4
constexpr size_t CP_FRACTION_2432_8192 = 2432;  // 2432/8192 = 19/64
constexpr size_t CP_FRACTION_3072_8192 = 3072;  // 3072/8192 = 3/8
constexpr size_t CP_FRACTION_3648_8192 = 3648;  // 3648/8192 = 57/128
constexpr size_t CP_FRACTION_4096_8192 = 4096;  // 4096/8192 = 1/2

// LDPC codeword lengths
constexpr size_t LDPC_SHORT_CODEWORD = 16200;
constexpr size_t LDPC_LONG_CODEWORD = 64800;

}  // namespace constants

}  // namespace atsc3
