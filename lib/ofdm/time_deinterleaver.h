// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Time De-interleavers — inverse of the A/322 Section 7.1 time interleaver
//
// CTI mode (7.1.4): the transmitter's Convolutional Time Interleaver has
// Nrows delay lines, line k holding k cells, with input and output
// commutators stepping one line per cell. The de-interleaver mirrors it with
// line k holding Nrows-1-k cells, so every cell is delayed Nrows*(Nrows-1)
// cells end to end. The commutator is placed at L1D_plp_CTI_start_row at the
// first cell of the PLP in a Subframe; until the lines have filled, outputs
// are the transmitter's initial (PRBS) cells, not data.
//
// HTI mode (7.1.5), intra-Subframe interleaving (N_IU = 1): each TI Block
// of N_FEC_TI FEC Blocks went through the Cell Interleaver (optional), then
// the Twisted Block Interleaver: written column-wise into Ncells rows by
// N_FEC_TI_MAX columns, the first N_FEC_TI_MAX - N_FEC_TI columns virtual,
// and read diagonal-wise skipping virtual cells. The Convolutional Delay
// Line used for inter-Subframe interleaving (N_IU > 1) is not implemented:
// configure() rejects it.
//
// T is any copyable cell type.
//
// AXI4-S Interface Contract:
//   CTI: Input TDATA=cell TVALID TREADY; Output TDATA=cell TVALID TREADY
//        (one out per in, fixed delay)
//   HTI: Input TDATA=cell TVALID TREADY TLAST(TI Block);
//        Output TDATA=cell TVALID TREADY TLAST(FEC Block)
//
// Reference: ATSC A/322 Sections 7.1.4, 7.1.5, Table 9.26

#include "ofdm/cell_deinterleaver.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Table 9.26: Nrows for L1D_plp_CTI_depth; 0 for a reserved value.
size_t cti_num_rows(uint8_t depth_code, bool extended_interleaving);

template <typename T>
class CtiDeinterleaver {
public:
    // Allocates Nrows*(Nrows-1)/2 cells. Returns false for num_rows < 2.
    bool configure(size_t num_rows) {
        rows_ = 0;
        mem_.clear();
        if (num_rows < 2) {
            return false;
        }
        rows_ = num_rows;
        base_.assign(rows_, 0);
        pos_.assign(rows_, 0);
        size_t total = 0;
        for (size_t k = 0; k < rows_; k++) {
            base_[k] = total;
            total += rows_ - 1 - k;
        }
        mem_.assign(total, T{});
        row_ = 0;
        return true;
    }

    size_t num_rows() const {
        return rows_;
    }

    // Commutator position for the next cell (L1D_plp_CTI_start_row at the
    // first cell of the PLP in a Subframe).
    void set_commutator(size_t row) {
        row_ = rows_ ? row % rows_ : 0;
    }

    size_t commutator() const {
        return row_;
    }

    // End-to-end delay of a cell through interleaver and de-interleaver.
    size_t delay() const {
        return rows_ * (rows_ - 1);
    }

    void process(const T* in, T* out, size_t num_cells) {
        for (size_t i = 0; i < num_cells; i++) {
            const size_t len = rows_ - 1 - row_;
            if (len == 0) {
                out[i] = in[i];
            } else {
                T& slot = mem_[base_[row_] + pos_[row_]];
                out[i] = slot;
                slot = in[i];
                if (++pos_[row_] == len) {
                    pos_[row_] = 0;
                }
            }
            if (++row_ == rows_) {
                row_ = 0;
            }
        }
    }

private:
    size_t rows_ = 0;
    size_t row_ = 0;
    std::vector<T> mem_;
    std::vector<size_t> base_;
    std::vector<size_t> pos_;
};

struct HtiConfig {
    // Ncells: cells per FEC Block (Table 6.14)
    size_t cells_per_fec_block = 0;
    // N_FEC_TI_MAX (from L1D_plp_HTI_num_fec_blocks_max, see
    // hti_fec_blocks_ti_max())
    size_t fec_blocks_ti_max = 0;
    // L1D_plp_HTI_cell_interleaver
    bool cell_interleaver = false;
    // N_IU: L1D_plp_HTI_num_ti_blocks + 1 when L1D_plp_HTI_inter_subframe = 1,
    // else 1. Only 1 is supported.
    size_t interleaving_units = 1;
};

// Section 7.1.5.1: FEC Blocks in TI Block s of an interleaving frame of
// blocks_if FEC Blocks split into n_ti TI Blocks (smaller blocks first).
size_t hti_fec_blocks_in_ti_block(size_t blocks_if, size_t n_ti, size_t s);
// N_FEC_TI_MAX = ceil(NBLOCKS_IF_MAX / N_TI)
size_t hti_fec_blocks_ti_max(size_t blocks_if_max, size_t n_ti);

// Twisted Block Interleaver read order for one TI Block: the memory
// position (column-major, Ncells rows, virtual columns first) of each output
// cell, data cells only. positions has num_fec_blocks * rows entries.
void hti_tbi_read_positions(size_t rows, size_t columns_max, size_t num_fec_blocks,
                            uint32_t* positions);

template <typename T>
class HtiDeinterleaver {
public:
    bool configure(const HtiConfig& config) {
        config_ = config;
        configured_ = false;
        if (config.interleaving_units != 1 || config.cells_per_fec_block == 0 ||
            config.fec_blocks_ti_max == 0) {
            return false;
        }
        if (config.cell_interleaver && !cell_.set_config({config.cells_per_fec_block})) {
            return false;
        }
        const size_t cells = config.cells_per_fec_block * config.fec_blocks_ti_max;
        positions_.assign(cells, 0);
        block_.assign(cells, T{});
        configured_ = true;
        return true;
    }

    bool is_configured() const {
        return configured_;
    }

    // in: the num_fec_blocks * Ncells cells of one TI Block as received;
    // out: its FEC Blocks in order. Returns false if not configured or
    // num_fec_blocks is outside 1..N_FEC_TI_MAX.
    bool deinterleave_ti_block(const T* in, size_t num_fec_blocks, T* out) {
        if (!configured_ || num_fec_blocks == 0 || num_fec_blocks > config_.fec_blocks_ti_max) {
            return false;
        }
        const size_t rows = config_.cells_per_fec_block;
        const size_t base = (config_.fec_blocks_ti_max - num_fec_blocks) * rows;
        const size_t n = num_fec_blocks * rows;
        hti_tbi_read_positions(rows, config_.fec_blocks_ti_max, num_fec_blocks, positions_.data());
        T* dst = config_.cell_interleaver ? block_.data() : out;
        for (size_t j = 0; j < n; j++) {
            dst[positions_[j] - base] = in[j];
        }
        if (config_.cell_interleaver) {
            for (size_t r = 0; r < num_fec_blocks; r++) {
                cell_.deinterleave(block_.data() + r * rows, out + r * rows, r);
            }
        }
        return true;
    }

private:
    HtiConfig config_;
    bool configured_ = false;
    CellDeinterleaver cell_;
    std::vector<uint32_t> positions_;
    std::vector<T> block_;
};

}  // namespace ofdm
}  // namespace atsc3
