// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// L1 signaling FEC — receive side of A/322 Section 6.5
//
// L1-Basic (Ksig = 200) and each L1-Detail segment are scrambled, BCH
// encoded (16K BCH, 168 parity bits), zero padded into Kldpc LDPC
// information bits by the shortening pattern piS, LDPC encoded (16K 3/15
// Type A for L1-Basic and L1-Detail Modes 1-2, 16K 6/15 Type B for L1-Detail
// Modes 3-7), parity permuted (parity interleaver for Modes 3-7, then the
// group order piP), extended by repetition (Mode 1 only), punctured from the
// end, stripped of the padding, block interleaved into eta_MOD columns and
// bit-demultiplexed into cells (Sections 6.5.2.2-6.5.2.10).
//
// L1FecDecoder undoes that for one coded block, from the demapper's LLRs
// for its cells (eta_MOD per cell, cell order) to the Ksig descrambled
// information bits. Positive LLR means bit 0.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8 (LLR, cell order)    TVALID TREADY TLAST(coded block)
//   Output: TDATA=uint8 (one bit per byte)  TVALID TREADY TLAST(Ksig bits)
//
// Reference: ATSC A/322 Section 6.5, Tables 6.17-6.25

#include "config/atsc3_config.h"
#include "fec/bch_decoder.h"
#include "fec/ldpc_decoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace atsc3 {
namespace framing {

enum class L1Kind : uint8_t { BASIC, DETAIL };

struct L1FecParams {
    L1Kind kind = L1Kind::BASIC;
    int mode = 1;  // 1..7 (L1B_L1_Detail_fec_type + 1 for L1-Detail)
    config::CodeRate rate = config::CodeRate::RATE_3_15;
    // Constellation; NUC tables are those of the stated rate (Table 6.17)
    config::Modulation modulation = config::Modulation::QPSK;
    config::CodeRate constellation_rate = config::CodeRate::RATE_3_15;
    size_t eta = 2;  // bits per cell
    size_t ksig = 0;
    size_t nouter = 0;        // Ksig + 168
    size_t kldpc = 0;         // 3240 or 6480
    size_t nldpc_parity = 0;  // 12960 or 9720
    size_t npunc = 0;
    size_t nrepeat = 0;
    size_t nfec = 0;       // coded bits excluding repetition
    size_t num_cells = 0;  // (nfec + nrepeat) / eta
};

// Fills p for one coded block. Returns false for a mode outside 1..7 or a
// Ksig outside the range of Table 6.17 (L1-Basic: exactly 200).
bool l1_fec_params(L1Kind kind, int mode, size_t ksig, L1FecParams* p);

// Section 6.5.3.1: splitting of the L1-Detail bits into coded blocks.
struct L1DetailSegmentation {
    size_t num_blocks = 0;    // N_L1D_FECFRAME
    size_t ksig = 0;          // bits per block (including padding)
    size_t padding_bits = 0;  // K_L1D_PAD
};
L1DetailSegmentation l1_detail_segmentation(int mode, size_t detail_size_bytes);

struct L1FecResult {
    bool ldpc_converged = false;
    bool bch_ok = false;
    size_t bch_errors = 0;
};

class L1FecDecoder {
public:
    L1FecDecoder();
    ~L1FecDecoder();

    // llr: p.num_cells * p.eta LLRs; bits: p.ksig output bits. Returns
    // false if LDPC does not converge or BCH cannot correct the block; the
    // output is then not valid.
    bool decode(const L1FecParams& p, const int8_t* llr, uint8_t* bits,
                L1FecResult* result = nullptr);

private:
    std::unique_ptr<fec::LdpcDecoder> ldpc_3_15_;
    std::unique_ptr<fec::LdpcDecoder> ldpc_6_15_;
    fec::BchDecoder bch_;
    std::vector<int16_t> tx_;       // received bits in transmitted order
    std::vector<int16_t> word_;     // LDPC codeword, permuted parity order
    std::vector<int8_t> codeword_;  // LDPC codeword, encoder order
    std::vector<uint8_t> padded_;   // 1 where an LDPC information bit is padding
    std::vector<uint8_t> scratch_;  // unpadded information bits (BCH codeword)
    std::vector<uint8_t> bytes_;
};

}  // namespace framing
}  // namespace atsc3
