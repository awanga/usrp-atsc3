// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// time_deinterleaver.cc — A/322 time de-interleaver parameters and the
// Twisted Block Interleaver read order
//
// Reference: ATSC A/322 Sections 7.1.4, 7.1.5, Table 9.26

#include "time_deinterleaver.h"

namespace atsc3 {
namespace ofdm {

size_t cti_num_rows(uint8_t depth_code, bool extended_interleaving) {
    switch (depth_code) {
        case 0:
            return 512;
        case 1:
            return 724;
        case 2:
            return extended_interleaving ? 1254 : 887;
        case 3:
            return extended_interleaving ? 1448 : 1024;
        default:
            return 0;
    }
}

size_t hti_fec_blocks_in_ti_block(size_t blocks_if, size_t n_ti, size_t s) {
    if (n_ti == 0) {
        return 0;
    }
    const size_t small = blocks_if / n_ti;
    return s < n_ti - blocks_if % n_ti ? small : small + 1;
}

size_t hti_fec_blocks_ti_max(size_t blocks_if_max, size_t n_ti) {
    return n_ti == 0 ? 0 : (blocks_if_max + n_ti - 1) / n_ti;
}

void hti_tbi_read_positions(size_t rows, size_t columns_max, size_t num_fec_blocks,
                            uint32_t* positions) {
    // Section 7.1.5.4: R_i = i mod Nr, T_i = R_i mod Nc,
    // C_i = (T_i + floor(i / Nr)) mod Nc, theta_i = Nr C_i + R_i; virtual
    // cells (theta_i < N_FEC_TI_DIFF * Nr) are skipped.
    const size_t virtual_cells = (columns_max - num_fec_blocks) * rows;
    size_t out = 0;
    for (size_t i = 0; i < rows * columns_max; i++) {
        const size_t r = i % rows;
        const size_t c = (r % columns_max + i / rows) % columns_max;
        const size_t theta = rows * c + r;
        if (theta >= virtual_cells) {
            positions[out++] = static_cast<uint32_t>(theta);
        }
    }
}

}  // namespace ofdm
}  // namespace atsc3
