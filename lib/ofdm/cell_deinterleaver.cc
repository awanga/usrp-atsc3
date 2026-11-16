// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// cell_deinterleaver.cc — inverse A/322 HTI Cell Interleaver
//
// Reference: ATSC A/322 Section 7.1.5.2

#include "cell_deinterleaver.h"

namespace atsc3 {
namespace ofdm {

namespace {

// Feedback taps into S_i[Nd-2], indexed by Nd - 11.
constexpr uint32_t kTaps[5] = {
    (1u << 0) | (1u << 3),
    (1u << 0) | (1u << 2),
    (1u << 0) | (1u << 1) | (1u << 4) | (1u << 6),
    (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 9) | (1u << 11),
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 12),
};

uint32_t parity(uint32_t v) {
    uint32_t p = 0;
    while (v) {
        p ^= v & 1u;
        v >>= 1;
    }
    return p;
}

size_t ceil_log2(size_t n) {
    size_t b = 0;
    while ((size_t{1} << b) < n) {
        b++;
    }
    return b;
}

}  // namespace

CellDeinterleaver::CellDeinterleaver(const CellDeinterleaverConfig& config) {
    set_config(config);
}

bool CellDeinterleaver::set_config(const CellDeinterleaverConfig& config) {
    config_ = config;
    basic_.clear();
    nd_ = ceil_log2(config_.num_cells);
    if (config_.num_cells < 2 || nd_ < 11 || nd_ > 15) {
        nd_ = 0;
        return false;
    }
    const uint32_t taps = kTaps[nd_ - 11];
    const size_t lfsr_bits = nd_ - 1;
    basic_.reserve(config_.num_cells);
    uint32_t s = 0;  // S_i[Nd-2..0]
    for (size_t i = 0; i < (size_t{1} << nd_); i++) {
        if (i == 2) {
            s = 1;
        } else if (i > 2) {
            uint32_t feedback = parity(s & taps);
            s = (s >> 1) | (feedback << (lfsr_bits - 1));
        }
        uint32_t value = static_cast<uint32_t>((i % 2) << lfsr_bits) | s;
        if (value < config_.num_cells) {
            basic_.push_back(value);
        }
    }
    return true;
}

size_t CellDeinterleaver::shift(size_t r) const {
    size_t k = 0;
    size_t p = 0;
    for (size_t block = 0; block <= r; block++) {
        p = config_.num_cells;
        while (p >= config_.num_cells) {
            p = 0;
            for (size_t j = 0; j < nd_; j++) {
                p |= ((k >> j) & 1u) << (nd_ - 1 - j);
            }
            k++;
        }
    }
    return p;
}

}  // namespace ofdm
}  // namespace atsc3
