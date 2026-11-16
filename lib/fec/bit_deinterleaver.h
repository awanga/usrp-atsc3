// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Bit de-interleaver — inverse of the A/322 Section 6.2 bit interleaver and
// the Section 6.3.3 demultiplexer
//
// The transmitter takes the LDPC codeword (lambda) through
//   parity interleaver (Type B LDPC codes only, 6.2.1)
//   -> group-wise interleaver (6.2.2, pi(j) from Annex B)
//   -> block interleaver (Type A or B per Tables 6.8/6.9, 6.2.3)
//   -> demultiplexer (6.3.3): cell s carries bits q[eta*s] .. q[eta*s+eta-1]
// This class maps the LLRs of one FEC Block, in transmitted cell order (the
// demapper's output for Ninner/eta consecutive cells), back to codeword
// order, which is the column order of LdpcMatrixGenerator's H.
//
// The permutation is built once in configure(); deinterleave() only copies.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8 (LLR, cell order)     TVALID TREADY TLAST(FEC Block)
//   Output: TDATA=int8 (LLR, codeword order) TVALID TREADY TLAST(codeword)

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace fec {

enum class BlockInterleaverType : uint8_t { TYPE_A, TYPE_B };

// Tables 6.8 and 6.9. bits_per_cell is eta_MOD (2..12).
BlockInterleaverType block_interleaver_type(config::CodeRate rate, size_t bits_per_cell,
                                            bool short_codeword);

class BitDeinterleaver {
public:
    BitDeinterleaver() = default;

    // Returns false (and leaves the de-interleaver unconfigured) for a
    // combination A/322 does not define, e.g. 1024QAM with 16200-bit codes.
    bool configure(config::CodeRate rate, config::Modulation modulation, bool short_codeword);

    bool is_configured() const {
        return !source_.empty();
    }

    size_t codeword_length() const {
        return source_.size();
    }

    // in: codeword_length() LLRs in cell order; out: codeword order.
    void deinterleave(const int8_t* in, int8_t* out) const;

    // Index into the transmitted (cell-order) stream of codeword bit k.
    size_t source_index(size_t k) const {
        return source_[k];
    }

private:
    std::vector<uint32_t> source_;
};

}  // namespace fec
}  // namespace atsc3
