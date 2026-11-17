// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Preamble demodulation — A/322 Sections 7.2.5, 7.3, 8.1.4, 8.1.6
//
// Turns the DC-centred FFT output of each preamble symbol (FftConfig::
// dc_centered, guard interval already removed) into its data cells in
// transmitted order, ready for L1 demapping:
//   1. carriers k = 0 .. NoC-1 at bins config::first_carrier_bin + k;
//   2. channel at the preamble pilots (k mod Dx == 0, amplitude Table 8.6)
//      and the common continual pilots (A_CP, Table 8.5), H = Y x 1/X,
//      interpolated linearly across k (ChannelEstimator; preamble pilots
//      repeat every symbol, Dy = 1, so frequency interpolation suffices);
//   3. zero-forcing equalization of the data cells (Equalizer);
//   4. frequency de-interleaving (FreqDeinterleaver; Section 7.3 always
//      interleaves preamble symbols, l counted from the first one).
// The first preamble symbol uses the minimum NoC (Cred_coeff 4); later
// ones use L1B_preamble_reduced_carriers (Section 7.2.5.1). The cells of
// symbol 0 start with the L1-Basic cells, then L1-Detail (Section 7.2.5.2).
// Tone reservation (never on the first symbol) is not handled yet.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (ci16 fixed-point) FFT bins, DC-centred, TLAST(symbol)
//   Output: TDATA=cf32 (ci16 fixed-point) data cells, TLAST(symbol)
//
// Reference: ATSC A/322:2026 Sections 7.2.5, 7.3, 8.1, 8.3

#include "channel/channel_estimator.h"
#include "channel/equalizer.h"
#include "ofdm/freq_deinterleaver.h"
#include "ofdm/pilot_extractor.h"
#include "ofdm/preamble.h"
#include "types.h"

#include <cstddef>
#include <vector>

namespace atsc3 {
namespace framing {

struct PreambleDemodConfig {
    size_t fft_size = 8192;   // from preamble_structure (Table H.1.1)
    size_t dx = 16;           // preamble pilot spacing (Table H.1.1)
    unsigned later_cred = 0;  // L1B_preamble_reduced_carriers (symbols 1..)
};

class PreambleDemodulator {
public:
    // Throws std::invalid_argument for an FFT size / Dx pair Table H.1.1
    // does not use, or later_cred > 4.
    explicit PreambleDemodulator(const PreambleDemodConfig& config);

    // Start of a frame: the next symbol is preamble symbol 0.
    void reset();

    // Data cells of the next preamble symbol (num_cells() of them) from
    // its fft_size DC-centred bins. Returns the count written.
    size_t demodulate(const sample_t* bins, sample_t* cells);

    // Data cells of the next symbol to be demodulated.
    size_t num_cells() const;

    size_t symbol_index() const {
        return symbol_;
    }

private:
    struct SymbolPath {
        ofdm::PreambleLayout layout;
        std::vector<ofdm::PilotSymbol> pilots;  // sorted by carrier; refs fixed
        channel::ChannelEstimator estimator;
        channel::Equalizer equalizer;
        size_t first_bin = 0;
    };
    static void build(size_t fft_size, size_t dx, unsigned cred, SymbolPath* path);

    PreambleDemodConfig config_;
    SymbolPath first_;
    SymbolPath later_;
    ofdm::FreqDeinterleaver deinterleaver_;
    std::vector<sample_t> scratch_;
    size_t symbol_ = 0;
};

}  // namespace framing
}  // namespace atsc3
