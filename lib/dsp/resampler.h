// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Resampler — rational sample-rate conversion for the receive chain
//
// The receiver needs three rates: the radio's (N210: 100 MHz / integer
// decimation, e.g. 6.25 MS/s), the bootstrap's fixed 6.144 MS/s (A/321
// Section 5.1), and the post-bootstrap (bsr_coefficient + 16) x 0.384
// MS/s (A/322 Section 9.1; 6.912 MS/s for a 6 MHz channel). This is a
// receiver design choice, not specified by ATSC.
//
// Output sample k sits at input position x_k = x_0 + k M / L, where
// out/in = L / M in lowest terms. The position is kept exactly as an
// integer plus a fraction in units of 1/L, so the output never drifts
// against the input. Each output is a windowed-sinc interpolation at
// x_k: num_phases polyphase branches of taps_per_phase taps, the branch
// nearest to the fractional part of x_k (timing error <= 1/(2
// num_phases) input sample). The lowpass has its 6 dB point midway
// between passband_hz and stopband_hz (Kaiser window, beta from
// stopband_db); each branch is normalized to unit DC gain.
//
// Defaults: passband +-2.916 MHz (the 5.832 MHz occupied band of a 6
// MHz channel) and stopband from 3.334 MHz (the first image of that band
// when 6.25 MS/s is upsampled to 6.912 MS/s), 64 taps, 512 branches.
// Downsampling 6.25 to 6.144 MS/s needs no stopband for the in-band
// signal (6.144 - 2.916 > 3.125 MHz).
//
// Latency: output k needs inputs up to floor(x_k) + taps_per_phase / 2.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (ci16 fixed-point) TVALID TREADY (input rate)
//   Output: TDATA=cf32 (ci16 fixed-point) TVALID TREADY (output rate)

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace dsp {

struct ResamplerConfig {
    uint64_t input_rate_hz = 6250000;
    uint64_t output_rate_hz = 6144000;
    double passband_hz = 2.916e6;
    double stopband_hz = 3.334e6;
    double stopband_db = 60.0;
    size_t taps_per_phase = 64;  // even
    size_t num_phases = 512;
    // Input samples kept for set_position() into the past, beyond the
    // filter's own span.
    size_t history = 16384;
};

// An input-time position: integer sample index plus frac / den.
struct ResamplerPosition {
    uint64_t index = 0;
    uint64_t frac = 0;
    uint64_t den = 1;
};

class Resampler {
public:
    // Throws std::invalid_argument for zero rates, an odd or zero
    // taps_per_phase, zero num_phases, or passband >= stopband.
    explicit Resampler(const ResamplerConfig& config = ResamplerConfig());

    // Consume n input samples and write the outputs they complete into
    // out (at most max_out). Returns the number written; an output that
    // did not fit stays pending and comes out on the next call.
    size_t process(const sample_t* in, size_t n, sample_t* out, size_t max_out);

    // Make the next output sit at input position p (rounded to the
    // nearest 1/L). Positions older than the kept history return false
    // and change nothing.
    bool set_position(const ResamplerPosition& p);

    // Input position of the next output.
    ResamplerPosition next_position() const;

    // Input position of output k of a resampler that started at input
    // sample 0 with this L/M: k M / L.
    ResamplerPosition position_of_output(uint64_t k) const;

    void reset();

    uint64_t interpolation() const {  // L
        return l_;
    }
    uint64_t decimation() const {  // M
        return m_;
    }
    uint64_t inputs_consumed() const {
        return n_in_;
    }
    const ResamplerConfig& get_config() const {
        return config_;
    }

#ifdef ATSC3_FIXED_POINT
    using Coeff = int16_t;  // Q1.15
#else
    using Coeff = float;
#endif
    // Branch p, tap j multiplies input floor(x) - taps_per_phase / 2 + 1
    // + j for a fractional position p / num_phases.
    const std::vector<Coeff>& branch(size_t p) const {
        return coeffs_[p];
    }

private:
    sample_t input_at(uint64_t i) const;
    sample_t interpolate(uint64_t index, size_t phase) const;

    ResamplerConfig config_;
    uint64_t l_ = 1;
    uint64_t m_ = 1;
    std::vector<std::vector<Coeff>> coeffs_;
    std::vector<sample_t> ring_;  // the last ring_.size() inputs
    uint64_t n_in_ = 0;           // inputs consumed so far
    uint64_t pos_index_ = 0;      // next output position: index + frac / L
    uint64_t pos_frac_ = 0;
};

}  // namespace dsp
}  // namespace atsc3
