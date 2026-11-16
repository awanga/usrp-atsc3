// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Cell De-interleaver — inverse of the A/322 Section 7.1.5.2 Cell Interleaver
// (the first stage of the Hybrid Time Interleaver, used when
// L1D_plp_HTI_cell_interleaver = 1)
//
// The interleaver reads FEC Block r of a TI Block as d[q] = g[L_r(q)] with
//   L_r(q) = (L_0(q) + P(r)) mod Ncells,
// L_0 an Nd-bit LFSR sequence (toggle bit Nd-1, address check < Ncells) and
// P(r) the Nd-bit bit-reversal of a counter, skipping values >= Ncells;
// Nd = ceil(log2 Ncells). r restarts at 0 on each TI Block.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cell (one FEC Block, interleaved) TVALID TREADY TLAST(FEC Block)
//   Output: TDATA=cell (one FEC Block, in order)    TVALID TREADY TLAST(FEC Block)
//
// Reference: ATSC A/322 Section 7.1.5.2, Table 6.14

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

struct CellDeinterleaverConfig {
    // Ncells: cells per FEC Block (Table 6.14), 2025..32400
    size_t num_cells = 0;
};

class CellDeinterleaver {
public:
    explicit CellDeinterleaver(const CellDeinterleaverConfig& config = CellDeinterleaverConfig());

    // Returns false if num_cells needs an Nd outside 11..15 (no Table 6.14
    // FEC Block size does); the de-interleaver is then unconfigured.
    bool set_config(const CellDeinterleaverConfig& config);

    const CellDeinterleaverConfig& get_config() const {
        return config_;
    }

    bool is_configured() const {
        return !basic_.empty();
    }

    // L_0(q), q < num_cells
    uint32_t basic_permutation(size_t q) const {
        return basic_[q];
    }

    // P(r) for FEC Block r of a TI Block
    size_t shift(size_t r) const;

    // De-interleave FEC Block r of the current TI Block: out[L_r(q)] = in[q].
    template <typename T>
    void deinterleave(const T* in, T* out, size_t r) const;

private:
    CellDeinterleaverConfig config_;
    size_t nd_ = 0;
    std::vector<uint32_t> basic_;
};

template <typename T>
void CellDeinterleaver::deinterleave(const T* in, T* out, size_t r) const {
    if (!is_configured()) {
        return;
    }
    const size_t n = config_.num_cells;
    const size_t p = shift(r);
    for (size_t q = 0; q < n; q++) {
        size_t addr = basic_[q] + p;
        if (addr >= n) {
            addr -= n;
        }
        out[addr] = in[q];
    }
}

}  // namespace ofdm
}  // namespace atsc3
