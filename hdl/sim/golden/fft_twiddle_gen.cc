// fft_twiddle_gen.cc — twiddle-ROM generator for hdl/rtl/ofdm/fft_engine.v
//
// lib/ofdm/fft_engine.cc's FixedPointFftEngine::compute_twiddles() is a
// private implementation detail of a class defined entirely inside that
// .cc file (fft_engine.h only exposes the abstract FftEngine interface),
// so there is no class instance to link against and dump from the way
// timing_recovery_coeffs.vh's generator does. Unlike the polyphase
// interpolator's raised-cosine/Kaiser-window/normalize design, though,
// compute_twiddles() is a small, direct formula with no multi-step
// numerical process to diverge on -- this reproduces it exactly
// (same angle formula, same sign convention, same std::cos/sin, same
// float intermediate) and calls the real, shared
// atsc3::float_to_q15() (lib/types.h) for the final quantization step,
// so at least that piece is never duplicated.
//
// Forward direction only (sign = -1): config/hdl_register_map.json ties
// fft_engine's DIRECTION to "const"/kForward -- "Receiver is
// forward-FFT-only in RTL scope" -- so fft_engine.v has no direction
// input and this generator never produces the inverse-sign table.
//
// Emits the master 16384-entry table (FFT_SIZE = 32768, i.e.
// FFT_SIZE/2 unique twiddles): fft_engine.v's header comment derives why
// this one table, addressed as rom[j << (15 - stage)], serves every
// smaller supported FFT_SIZE (8192, 16384) without a separate table or
// any runtime rescaling.
//
// Output (stdout): one line per index k = 0..16383:
//   <k> <cos_q15> <neg_sin_q15>
//
// Usage: fft_twiddle_gen > fft_twiddles.vh.raw (then reformatted into
// fft_twiddles.vh's `coeff_rom[..] = ...;` assignments -- see that
// file's header)

#include "types.h"

#include <cmath>
#include <cstdio>

// atsc3::float_to_q15() only exists under ATSC3_FIXED_POINT (lib/types.h);
// this CLI is built unconditionally in both build/ and build-fxp/, so the
// float build needs its own branch even though it can't produce anything
// meaningful (the RTL this feeds is fixed-point-only).
int main() {
#ifdef ATSC3_FIXED_POINT
    constexpr size_t kMaxFftSize = 32768;
    constexpr size_t kNumTwiddles = kMaxFftSize / 2;
    constexpr double kSign = -1.0;  // forward direction only

    for (size_t k = 0; k < kNumTwiddles; ++k) {
        double angle =
            kSign * 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(kMaxFftSize);
        int16_t re = atsc3::float_to_q15(static_cast<float>(std::cos(angle)));
        int16_t im = atsc3::float_to_q15(static_cast<float>(std::sin(angle)));
        std::printf("%zu %d %d\n", k, re, im);
    }
    return 0;
#else
    std::fprintf(stderr, "fft_twiddle_gen requires -DATSC3_FIXED_POINT=ON\n");
    return 1;
#endif
}
