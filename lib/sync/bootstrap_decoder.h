// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Bootstrap Decoder — A/321 bootstrap signaling recovery (version 0.0)
//
// Works on one whole bootstrap (4 x 3072 samples at 6.144 MS/s) whose start
// the time-domain detector has found to within +-260 samples and whose
// fractional frequency offset has been removed (residual below ~1/6 of the
// 3 kHz subcarrier spacing). For each symbol it takes a 2048-sample window
// that starts 260 samples ahead of part A, so it stays inside the
// 520-sample cyclic part C for any timing error in that range, transforms
// it, multiplies by the conjugate of the symbol's known ZC x PN sequence
// and transforms back. The position of the resulting peak is the symbol's
// cyclic shift (plus the common timing error), so:
//   - symbol 0's peak gives the timing error (its cyclic shift is 0);
//   - the difference between consecutive peaks is the Gray-coded relative
//     cyclic shift carrying the 8 signaling bits (A/321 5.3.2, Annex A);
//   - the sign of each peak against the previous one marks the phase-
//     inverted last symbol (A/321 5.2.3).
// Symbol 0 is also correlated against the reference shifted by
// -max..+max bins to find the integer frequency offset.
//
// A frame is accepted only if every symbol's peak stands out of the
// correlation floor (peak power >= min_peak_to_mean x mean power), only
// the last symbol is inverted, and the signaled values are allowed
// (parse_bootstrap_signaling). Bootstraps of other versions do not
// correlate and are rejected.
//
// Both transforms use a normalized 2048-point inverse FFT (the forward
// transform as conj(IFFT(conj x)) = FFT(x)/N), so the fixed-point build
// keeps the Q1.15 bins in range without a separate scaling stage.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (ci16 fixed-point), TVALID, TREADY, TLAST(bootstrap)
//           12288 samples from the coarse start of symbol 0
//   Output: decode result (fields, timing, integer CFO) per bootstrap
//
// Reference: ATSC A/321:2026 Sections 5.2-5.4, 6.1, Annex A

#include "ofdm/fft_engine.h"
#include "sync/bootstrap_signal.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace atsc3 {
namespace sync {

struct BootstrapDecoderConfig {
    int max_integer_cfo_bins = 2;
    // Peak power over mean correlation power required of every symbol.
    // Noise alone peaks near ln(2048) ~ 7.6 times the mean.
    double min_peak_to_mean = 20.0;
};

struct BootstrapDecodeResult {
    bool valid = false;           // correlated && termination_ok && fields_ok
    bool correlated = false;      // every symbol above min_peak_to_mean
    bool termination_ok = false;  // only the last symbol phase inverted
    bool fields_ok = false;       // parse_bootstrap_signaling accepted them
    // Start of symbol 0 relative to the first input sample.
    int timing_offset = 0;
    int integer_cfo_bins = 0;
    uint16_t relative_shift[kBootstrapSymbols - 1] = {};
    uint8_t fields[kBootstrapSymbols - 1] = {};
    BootstrapSignaling signaling;
};

class BootstrapDecoder {
public:
    static constexpr size_t kGuard = 260;
    static constexpr size_t kInputLength = kBootstrapSymbols * kBootstrapSymbolLength;

    explicit BootstrapDecoder(const BootstrapDecoderConfig& config = BootstrapDecoderConfig());
    ~BootstrapDecoder();

    // in: kInputLength samples. Returns out->valid.
    bool decode(const sample_t* in, BootstrapDecodeResult* out);

    const BootstrapDecoderConfig& get_config() const {
        return config_;
    }

private:
#ifdef ATSC3_FIXED_POINT
    using Power = int64_t;
#else
    using Power = double;
#endif
    struct Peak {
        size_t index = 0;
        sample_t value{};
        Power power{};
        Power total{};
    };

    // Y = FFT(window)/N into bins_.
    void transform(const sample_t* window);
    // Correlates bins_ shifted by d against symbol n's reference.
    Peak correlate(size_t n, int d);
    bool peak_stands_out(const Peak& p) const;

    BootstrapDecoderConfig config_;
    std::unique_ptr<ofdm::FftEngine> ifft_;
    std::vector<std::vector<sample_t>> reference_;  // conj(s_n(k)), per symbol
    std::vector<sample_t> work_;
    std::vector<sample_t> bins_;
    std::vector<sample_t> corr_;
};

}  // namespace sync
}  // namespace atsc3
