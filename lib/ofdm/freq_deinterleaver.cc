// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// freq_deinterleaver.cc — inverse A/322 frequency interleaver
//
// Reference: ATSC A/322 Section 7.3, Tables 7.11-7.14

#include "freq_deinterleaver.h"

#include <initializer_list>

namespace atsc3 {
namespace ofdm {

namespace {

struct FiParams {
    size_t nr;                 // log2(Mmax)
    uint32_t r_taps;           // R'_{i-1} bits XORed into R'_i[Nr-2]
    uint32_t g_taps;           // G_{k-1} bits XORed into G_k[Nr-1]
    const uint8_t* wire_even;  // R_i bit position for R'_i bit Nr-2 .. 0
    const uint8_t* wire_odd;
};

constexpr uint32_t bits(std::initializer_list<int> positions) {
    uint32_t v = 0;
    for (int p : positions) {
        v |= 1u << p;
    }
    return v;
}

// Tables 7.12-7.14, listed for R'_i bit positions from Nr-2 down to 0.
constexpr uint8_t kWire8kEven[] = {5, 11, 3, 0, 10, 8, 6, 9, 2, 4, 1, 7};
constexpr uint8_t kWire8kOdd[] = {8, 10, 7, 6, 0, 5, 2, 1, 3, 9, 4, 11};
constexpr uint8_t kWire16kEven[] = {8, 4, 3, 2, 0, 11, 1, 5, 12, 10, 6, 7, 9};
constexpr uint8_t kWire16kOdd[] = {7, 9, 5, 3, 11, 1, 4, 0, 2, 12, 10, 8, 6};
constexpr uint8_t kWire32k[] = {6, 5, 0, 10, 8, 1, 11, 12, 2, 9, 4, 3, 13, 7};

bool params_for(size_t fft_size, FiParams* p) {
    switch (fft_size) {
        case 8192:
            *p = {13, bits({0, 1, 4, 6}), bits({0, 1, 4, 5, 9, 11}), kWire8kEven, kWire8kOdd};
            return true;
        case 16384:
            *p = {14, bits({0, 1, 4, 5, 9, 11}), bits({0, 1, 2, 12}), kWire16kEven, kWire16kOdd};
            return true;
        case 32768:
            *p = {15, bits({0, 1, 2, 12}), bits({0, 1}), kWire32k, kWire32k};
            return true;
        default:
            return false;
    }
}

uint32_t parity(uint32_t v) {
    uint32_t p = 0;
    while (v) {
        p ^= v & 1u;
        v >>= 1;
    }
    return p;
}

// Section 7.3: (i mod 2) 2^(Nr-1) + R_i, i < Mmax, for one wiring.
std::vector<uint32_t> raw_sequence(const FiParams& fp, const uint8_t* wire) {
    const size_t mmax = size_t{1} << fp.nr;
    const size_t rbits = fp.nr - 1;
    std::vector<uint32_t> raw(mmax);
    uint32_t r_prime = 0;
    for (size_t i = 0; i < mmax; i++) {
        if (i == 2) {
            r_prime = 1;
        } else if (i > 2) {
            uint32_t feedback = parity(r_prime & fp.r_taps);
            r_prime = (r_prime >> 1) | (feedback << (rbits - 1));
        }
        uint32_t r = 0;
        for (size_t k = 0; k < rbits; k++) {
            size_t from = rbits - 1 - k;
            r |= ((r_prime >> from) & 1u) << wire[k];
        }
        raw[i] = static_cast<uint32_t>((i % 2) << rbits) | r;
    }
    return raw;
}

}  // namespace

FreqDeinterleaver::FreqDeinterleaver(const FreqDeinterleaverConfig& config) {
    set_config(config);
}

bool FreqDeinterleaver::set_config(const FreqDeinterleaverConfig& config) {
    config_ = config;
    symbol_ = 0;
    raw_even_.clear();
    raw_odd_.clear();
    FiParams fp{};
    if (!params_for(config_.fft_size, &fp)) {
        nr_ = 0;
        h_.clear();
        return false;
    }
    nr_ = fp.nr;
    raw_even_ = raw_sequence(fp, fp.wire_even);
    raw_odd_ = raw_sequence(fp, fp.wire_odd);
    h_.assign(config_.fft_size, 0);
    return true;
}

uint32_t FreqDeinterleaver::symbol_offset(size_t k) const {
    FiParams fp{};
    params_for(config_.fft_size, &fp);
    uint32_t g = (1u << nr_) - 1;
    for (size_t step = 0; step < k; step++) {
        uint32_t feedback = parity(g & fp.g_taps);
        g = (g >> 1) | (feedback << (nr_ - 1));
    }
    return g;
}

void FreqDeinterleaver::interleaving_sequence(size_t l, size_t num_cells, uint32_t* h) const {
    if (!is_configured()) {
        return;
    }
    const std::vector<uint32_t>& raw = (l % 2 == 0) ? raw_even_ : raw_odd_;
    const uint32_t g = symbol_offset(l / 2);
    size_t p = 0;
    for (size_t i = 0; i < raw.size() && p < num_cells; i++) {
        uint32_t addr = raw[i] ^ g;
        if (addr < num_cells) {
            h[p++] = addr;
        }
    }
}

}  // namespace ofdm
}  // namespace atsc3
