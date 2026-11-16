// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// l1_fec.cc — A/322 L1 signaling FEC decoding
//
// Reference: ATSC A/322 Section 6.5, Tables 6.17-6.25

#include "l1_fec.h"

#include "framing/baseband_packet.h"

#include <algorithm>

namespace atsc3 {
namespace framing {

namespace {

constexpr size_t kGroup = 360;
constexpr size_t kMouter = 168;

struct ModeTable {
    size_t kldpc;
    size_t ksig_max;
    size_t a_num, a_den, b;  // Table 6.24: Npunc_temp = floor(A (Kldpc - Nouter)) + B
    size_t eta;
    config::Modulation modulation;
    config::CodeRate constellation_rate;
    const uint8_t* shortening;  // Table 6.20, Kldpc/360 entries
    const uint8_t* parity;      // Tables 6.21/6.22, pi_P(j) for j >= Kldpc/360
};

// Table 6.20
constexpr uint8_t kShortBasic[9] = {4, 1, 5, 2, 8, 6, 0, 7, 3};
constexpr uint8_t kShortD1[9] = {7, 8, 5, 4, 1, 2, 6, 3, 0};
constexpr uint8_t kShortD2[9] = {6, 1, 7, 8, 0, 2, 4, 3, 5};
constexpr uint8_t kShortD3[18] = {0, 12, 15, 13, 2, 5, 7, 9, 8, 6, 16, 10, 14, 1, 17, 11, 4, 3};
constexpr uint8_t kShortD4[18] = {0, 15, 5, 16, 17, 1, 6, 13, 11, 4, 7, 12, 8, 14, 2, 3, 9, 10};
constexpr uint8_t kShortD5[18] = {2, 4, 5, 17, 9, 7, 1, 6, 15, 8, 10, 14, 16, 0, 11, 13, 12, 3};
constexpr uint8_t kShortD7[18] = {15, 7, 8, 11, 5, 10, 16, 4, 12, 3, 0, 6, 9, 1, 14, 17, 2, 13};

// Table 6.21 (j = 9..44)
constexpr uint8_t kParityBasic[36] = {20, 23, 25, 32, 38, 41, 18, 9,  10, 11, 31, 24,
                                      14, 15, 26, 40, 33, 19, 28, 34, 16, 39, 27, 30,
                                      21, 44, 43, 35, 42, 36, 12, 13, 29, 22, 37, 17};
constexpr uint8_t kParityD1[36] = {16, 22, 27, 30, 37, 44, 20, 23, 25, 32, 38, 41,
                                   9,  10, 17, 18, 21, 33, 35, 14, 28, 12, 15, 19,
                                   11, 24, 29, 34, 36, 13, 40, 43, 31, 26, 39, 42};
constexpr uint8_t kParityD2[36] = {9,  31, 23, 10, 11, 25, 43, 29, 36, 16, 27, 34,
                                   26, 18, 37, 15, 13, 17, 35, 21, 20, 24, 44, 12,
                                   22, 40, 19, 32, 38, 41, 30, 33, 14, 28, 39, 42};
// Table 6.22 (j = 18..44)
constexpr uint8_t kParityD3[27] = {19, 37, 30, 42, 23, 44, 27, 40, 21, 34, 25, 32, 29, 24,
                                   26, 35, 39, 20, 18, 43, 31, 36, 38, 22, 33, 28, 41};
constexpr uint8_t kParityD4[27] = {20, 35, 42, 39, 26, 23, 30, 18, 28, 37, 32, 27, 44, 43,
                                   41, 40, 38, 36, 34, 33, 31, 29, 25, 24, 22, 21, 19};
constexpr uint8_t kParityD5[27] = {19, 37, 33, 26, 40, 43, 22, 29, 24, 35, 44, 31, 27, 20,
                                   21, 39, 25, 42, 34, 18, 32, 38, 23, 30, 28, 36, 41};
constexpr uint8_t kParityD7[27] = {44, 23, 29, 33, 24, 28, 21, 27, 42, 18, 22, 31, 32, 37,
                                   43, 30, 25, 35, 20, 34, 39, 36, 19, 41, 40, 26, 38};

using config::CodeRate;
using config::Modulation;

// Table 6.17 constellations, shared by L1-Basic and L1-Detail modes.
constexpr Modulation kMod[7] = {Modulation::QPSK,   Modulation::QPSK,   Modulation::QPSK,
                                Modulation::NUC_16, Modulation::NUC_64, Modulation::NUC_256,
                                Modulation::NUC_256};
constexpr CodeRate kModRate[7] = {CodeRate::RATE_3_15, CodeRate::RATE_3_15, CodeRate::RATE_3_15,
                                  CodeRate::RATE_8_15, CodeRate::RATE_9_15, CodeRate::RATE_9_15,
                                  CodeRate::RATE_13_15};
constexpr size_t kEta[7] = {2, 2, 2, 4, 6, 8, 8};

// Table 6.24 B for L1-Basic (A = 0).
constexpr size_t kBasicB[7] = {9360, 11460, 12360, 12292, 12350, 12432, 12776};

bool mode_table(L1Kind kind, int mode, ModeTable* t) {
    if (mode < 1 || mode > 7) {
        return false;
    }
    const size_t m = static_cast<size_t>(mode - 1);
    if (kind == L1Kind::BASIC) {
        *t = {3240,    200,     0,           1,           kBasicB[m],
              kEta[m], kMod[m], kModRate[m], kShortBasic, kParityBasic};
        return true;
    }
    switch (mode) {
        case 1:
            *t = {3240, 2352, 7, 2, 0, 2, kMod[m], kModRate[m], kShortD1, kParityD1};
            break;
        case 2:
            *t = {3240, 3072, 2, 1, 6036, 2, kMod[m], kModRate[m], kShortD2, kParityD2};
            break;
        case 3:
            *t = {6480, 6312, 11, 16, 4653, 2, kMod[m], kModRate[m], kShortD3, kParityD3};
            break;
        case 4:
            *t = {6480, 6312, 29, 32, 3200, 4, kMod[m], kModRate[m], kShortD4, kParityD4};
            break;
        case 5:
            *t = {6480, 6312, 3, 4, 4284, 6, kMod[m], kModRate[m], kShortD5, kParityD5};
            break;
        case 6:
            *t = {6480, 6312, 11, 16, 4900, 8, kMod[m], kModRate[m], kShortD4, kParityD4};
            break;
        default:
            *t = {6480, 6312, 49, 256, 8246, 8, kMod[m], kModRate[m], kShortD7, kParityD7};
            break;
    }
    return true;
}

int8_t saturate(int v) {
    return static_cast<int8_t>(std::max(-127, std::min(127, v)));
}

}  // namespace

bool l1_fec_params(L1Kind kind, int mode, size_t ksig, L1FecParams* p) {
    ModeTable t{};
    if (!mode_table(kind, mode, &t)) {
        return false;
    }
    const size_t ksig_min = 200;
    // Ksig is whole bytes: 200 for L1-Basic, and segmentation pads each
    // L1-Detail block to a byte multiple (6.5.3.1).
    if (ksig < ksig_min || ksig > t.ksig_max || ksig % 8 != 0) {
        return false;
    }
    p->kind = kind;
    p->mode = mode;
    p->rate = t.kldpc == 3240 ? CodeRate::RATE_3_15 : CodeRate::RATE_6_15;
    p->modulation = t.modulation;
    p->constellation_rate = t.constellation_rate;
    p->eta = t.eta;
    p->ksig = ksig;
    p->nouter = ksig + kMouter;
    p->kldpc = t.kldpc;
    p->nldpc_parity = 16200 - t.kldpc;

    // Section 6.5.2.7 (Table 6.23): L1-Basic Mode 1 C = 0, D = 3672;
    // L1-Detail Mode 1 C = 61/16, D = -508.
    p->nrepeat = 0;
    if (mode == 1) {
        p->nrepeat = kind == L1Kind::BASIC ? 3672 : 2 * (61 * p->nouter / 16) - 508;
    }
    // Section 6.5.2.8 steps 1-4.
    const size_t npunc_temp = t.a_num * (t.kldpc - p->nouter) / t.a_den + t.b;
    const size_t nfec_temp = p->nouter + p->nldpc_parity - npunc_temp;
    p->nfec = (nfec_temp + t.eta - 1) / t.eta * t.eta;
    p->npunc = npunc_temp - (p->nfec - nfec_temp);
    p->num_cells = (p->nfec + p->nrepeat) / t.eta;
    return true;
}

L1DetailSegmentation l1_detail_segmentation(int mode, size_t detail_size_bytes) {
    L1DetailSegmentation s;
    ModeTable t{};
    if (!mode_table(L1Kind::DETAIL, mode, &t) || detail_size_bytes == 0) {
        return s;
    }
    // Table 6.25: Kseg equals the Ksig limit of Table 6.17.
    const size_t kseg = t.ksig_max;
    const size_t k_ex = 8 * detail_size_bytes;
    s.num_blocks = (k_ex + kseg - 1) / kseg;
    const size_t per_block_bytes = (k_ex + 8 * s.num_blocks - 1) / (8 * s.num_blocks);
    s.padding_bits = per_block_bytes * 8 * s.num_blocks - k_ex;
    s.ksig = (k_ex + s.padding_bits) / s.num_blocks;
    return s;
}

L1FecDecoder::L1FecDecoder() : bch_(fec::get_atsc3_bch_config(CodeRate::RATE_3_15, true)) {
    fec::LdpcConfig c;
    c.short_codeword = true;
    c.code_rate = CodeRate::RATE_3_15;
    ldpc_3_15_ = std::make_unique<fec::LdpcDecoder>(c);
    c.code_rate = CodeRate::RATE_6_15;
    ldpc_6_15_ = std::make_unique<fec::LdpcDecoder>(c);
    tx_.reserve(16200 + 16200);
    word_.reserve(16200);
    codeword_.reserve(16200);
    padded_.reserve(6480);
    scratch_.reserve(6480);
    bytes_.reserve(6480 / 8);
}

L1FecDecoder::~L1FecDecoder() = default;

bool L1FecDecoder::decode(const L1FecParams& p, const int8_t* llr, uint8_t* bits,
                          L1FecResult* result) {
    L1FecResult local;
    L1FecResult& r = result ? *result : local;
    r = L1FecResult{};
    ModeTable t{};
    if (!mode_table(p.kind, p.mode, &t) || p.kldpc != t.kldpc) {
        return false;
    }
    const size_t eta = p.eta;
    const size_t ntx = p.nfec + p.nrepeat;
    const size_t rows = ntx / eta;

    // Section 6.5.2.10: bits written column-wise into eta columns, read
    // row-wise; for eta > 2 row i is cyclically shifted: c_i(j) =
    // b_i((i + j) mod eta).
    tx_.assign(ntx, 0);
    for (size_t i = 0; i < rows; i++) {
        for (size_t j = 0; j < eta; j++) {
            size_t col = eta == 2 ? j : (i + j) % eta;
            tx_[col * rows + i] = llr[i * eta + j];
        }
    }

    // Transmitted order: Nouter unpadded information bits, Nrepeat repeated
    // parity bits, then the parity bits that survive puncturing.
    const size_t n = 16200;
    word_.assign(n, 0);
    const int kKnownZero = 127;

    // Zero padding (Section 6.5.2.4): groups piS(0..Npad-1) and the first
    // bits of group piS(Npad) are padded; the rest take the BCH bits in order.
    padded_.assign(p.kldpc, 0);
    const size_t pad_bits = p.kldpc - p.nouter;
    const size_t npad = pad_bits / kGroup;
    for (size_t g = 0; g < npad; g++) {
        std::fill_n(padded_.begin() + static_cast<std::ptrdiff_t>(t.shortening[g] * kGroup), kGroup,
                    1);
    }
    if (npad < p.kldpc / kGroup) {
        std::fill_n(padded_.begin() + static_cast<std::ptrdiff_t>(t.shortening[npad] * kGroup),
                    pad_bits - npad * kGroup, 1);
    }
    size_t src = 0;
    for (size_t k = 0; k < p.kldpc; k++) {
        word_[k] = static_cast<int16_t>(padded_[k] ? kKnownZero : tx_[src++]);
    }
    // Repetition (6.5.2.7): repeated bit q is parity bit q mod Nparity.
    for (size_t q = 0; q < p.nrepeat; q++) {
        word_[p.kldpc + q % p.nldpc_parity] += tx_[p.nouter + q];
    }
    // Puncturing (6.5.2.8) removed the last Npunc parity bits.
    for (size_t q = 0; q + p.npunc < p.nldpc_parity; q++) {
        word_[p.kldpc + q] += tx_[p.nouter + p.nrepeat + q];
    }

    // Parity permutation (6.5.2.6), inverted: Y_j = X_piP(j) for the parity
    // groups, then (Modes 3-7) u[K + 360t + s] = c[K + 27s + t].
    const size_t info_groups = p.kldpc / kGroup;
    const size_t qldpc = p.nldpc_parity / kGroup;
    const bool parity_interleaved = p.kind == L1Kind::DETAIL && p.mode >= 3;
    codeword_.assign(n, 0);
    for (size_t k = 0; k < p.kldpc; k++) {
        codeword_[k] = saturate(word_[k]);
    }
    for (size_t j = info_groups; j < n / kGroup; j++) {
        const size_t xg = t.parity[j - info_groups];  // X group carried as Y_j
        for (size_t b = 0; b < kGroup; b++) {
            size_t u = xg * kGroup + b;
            size_t c = u;
            if (parity_interleaved) {
                size_t off = u - p.kldpc;
                c = p.kldpc + qldpc * (off % kGroup) + off / kGroup;
            }
            codeword_[c] = saturate(word_[j * kGroup + b]);
        }
    }

    fec::LdpcDecoder& ldpc = p.rate == CodeRate::RATE_3_15 ? *ldpc_3_15_ : *ldpc_6_15_;
    fec::LdpcResult lr = ldpc.decode(codeword_.data());
    r.ldpc_converged = lr.converged;
    if (!lr.converged) {
        return false;
    }

    // Unpadded information bits -> BCH (shortened 16K code, Nouter bits).
    const std::vector<uint8_t>& info = lr.decoded_bits;
    scratch_.clear();
    for (size_t k = 0; k < p.kldpc; k++) {
        if (!padded_[k]) {
            scratch_.push_back(info[k]);
        }
    }
    fec::BchConfig bc;
    bc.m = 14;
    bc.t = 12;
    bc.codeword_length = p.nouter;
    bc.info_length = p.ksig;
    bch_.set_config(bc);
    int errors = bch_.decode(scratch_.data(), p.nouter, bits);
    if (errors < 0) {
        return false;
    }
    r.bch_ok = true;
    r.bch_errors = static_cast<size_t>(errors);

    // Descramble (6.5.2.2: the Baseband Packet scrambler over Ksig bits).
    bytes_.resize(p.ksig / 8);
    pack_bits_msb_first(bits, p.ksig, bytes_.data());
    bbp_scramble(bytes_.data(), bytes_.size());
    for (size_t i = 0; i < p.ksig; i++) {
        bits[i] = (bytes_[i / 8] >> (7 - i % 8)) & 1u;
    }
    return true;
}

}  // namespace framing
}  // namespace atsc3
