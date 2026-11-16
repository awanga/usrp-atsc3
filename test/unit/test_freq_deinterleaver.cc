// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_freq_deinterleaver.cc — Unit tests for lib/ofdm/freq_deinterleaver.h
//
// The reference is the A/322 Section 7.3 frequency interleaver written out
// literally from the text: bit-array registers R' and G updated by the
// printed recurrences, the wire permutations as the two rows of Tables
// 7.12-7.14, the H_l(p) loop with its address check, and the printed
// input/output relations. The library must invert it.

#include "ofdm/freq_deinterleaver.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

struct Spec {
    int nr;
    std::vector<int> r_feedback;  // R'_i[Nr-2] = XOR of these R'_{i-1} bits
    std::vector<int> g_feedback;  // G_k[Nr-1] = XOR of these G_{k-1} bits
    std::vector<int> r_prime_row; // "R'i bit positions" row
    std::vector<int> even_row;    // "Ri bit positions (even)" row
    std::vector<int> odd_row;     // "(odd)" row, or the single 32K row
};

Spec spec_for(size_t fft) {
    if (fft == 8192) {
        return {13,
                {0, 1, 4, 6},
                {0, 1, 4, 5, 9, 11},
                {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
                {5, 11, 3, 0, 10, 8, 6, 9, 2, 4, 1, 7},
                {8, 10, 7, 6, 0, 5, 2, 1, 3, 9, 4, 11}};
    }
    if (fft == 16384) {
        return {14,
                {0, 1, 4, 5, 9, 11},
                {0, 1, 2, 12},
                {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
                {8, 4, 3, 2, 0, 11, 1, 5, 12, 10, 6, 7, 9},
                {7, 9, 5, 3, 11, 1, 4, 0, 2, 12, 10, 8, 6}};
    }
    return {15,
            {0, 1, 2, 12},
            {0, 1},
            {13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
            {6, 5, 0, 10, 8, 1, 11, 12, 2, 9, 4, 3, 13, 7},
            {6, 5, 0, 10, 8, 1, 11, 12, 2, 9, 4, 3, 13, 7}};
}

std::vector<uint32_t> spec_h(size_t fft, size_t l, size_t n_data) {
    Spec s = spec_for(fft);
    const int nr = s.nr;
    const size_t mmax = size_t{1} << nr;
    // G_k, k = floor(l/2): G_0 all ones, then shift down with feedback.
    std::vector<int> g(static_cast<size_t>(nr), 1);
    for (size_t k = 1; k <= l / 2; k++) {
        std::vector<int> next(static_cast<size_t>(nr));
        for (int j = 0; j <= nr - 2; j++) {
            next[static_cast<size_t>(j)] = g[static_cast<size_t>(j + 1)];
        }
        int fb = 0;
        for (int t : s.g_feedback) {
            fb ^= g[static_cast<size_t>(t)];
        }
        next[static_cast<size_t>(nr - 1)] = fb;
        g = next;
    }
    const std::vector<int>& to_row = (fft != 32768 && l % 2 == 1) ? s.odd_row : s.even_row;

    std::vector<int> rp(static_cast<size_t>(nr - 1), 0);
    std::vector<uint32_t> h;
    for (size_t i = 0; i < mmax; i++) {
        if (i == 2) {
            rp.assign(rp.size(), 0);
            rp[0] = 1;
        } else if (i > 2) {
            std::vector<int> next(rp.size());
            for (int j = 0; j <= nr - 3; j++) {
                next[static_cast<size_t>(j)] = rp[static_cast<size_t>(j + 1)];
            }
            int fb = 0;
            for (int t : s.r_feedback) {
                fb ^= rp[static_cast<size_t>(t)];
            }
            next[static_cast<size_t>(nr - 2)] = fb;
            rp = next;
        }
        std::vector<int> r(static_cast<size_t>(nr - 1), 0);
        for (size_t c = 0; c < s.r_prime_row.size(); c++) {
            r[static_cast<size_t>(to_row[c])] = rp[static_cast<size_t>(s.r_prime_row[c])];
        }
        uint32_t addr = static_cast<uint32_t>(i % 2) << (nr - 1);
        for (int j = 0; j <= nr - 2; j++) {
            addr += static_cast<uint32_t>(r[static_cast<size_t>(j)]) << j;
        }
        uint32_t goff = 0;
        for (int j = 0; j <= nr - 1; j++) {
            goff += static_cast<uint32_t>(g[static_cast<size_t>(j)]) << j;
        }
        addr ^= goff;
        if (addr < n_data) {
            h.push_back(addr);
        }
    }
    return h;
}

// Transmitter: x (input cells) -> a (output cells) for symbol l.
std::vector<uint32_t> spec_interleave(const std::vector<uint32_t>& x, size_t fft, size_t l) {
    auto h = spec_h(fft, l, x.size());
    std::vector<uint32_t> a(x.size());
    for (size_t p = 0; p < x.size(); p++) {
        if (fft == 32768 && l % 2 == 0) {
            a[h[p]] = x[p];
        } else {
            a[p] = x[h[p]];
        }
    }
    return a;
}

class FreqDeinterleaverSpec : public ::testing::TestWithParam<std::tuple<size_t, size_t>> {};

TEST_P(FreqDeinterleaverSpec, SequenceMatchesSpecForFirstSymbols) {
    auto [fft, n_data] = GetParam();
    FreqDeinterleaver d({fft});
    ASSERT_TRUE(d.is_configured());
    std::vector<uint32_t> h(n_data);
    // Early symbols, then later ones where the symbol offset registers have
    // run long enough to separate the feedback taps.
    for (size_t l : {0u, 1u, 2u, 3u, 4u, 5u, 41u, 128u, 255u}) {
        d.interleaving_sequence(l, n_data, h.data());
        ASSERT_EQ(h, spec_h(fft, l, n_data)) << "symbol " << l;
        std::vector<uint32_t> sorted(h);
        std::sort(sorted.begin(), sorted.end());
        for (size_t p = 0; p < n_data; p++) {
            ASSERT_EQ(sorted[p], p) << "H_" << l << " is not a permutation";
        }
    }
}

TEST_P(FreqDeinterleaverSpec, InvertsSpecInterleaverOverSymbols) {
    auto [fft, n_data] = GetParam();
    FreqDeinterleaver d({fft});
    std::vector<uint32_t> x(n_data), out(n_data);
    for (size_t l = 0; l < 5; l++) {
        std::iota(x.begin(), x.end(), static_cast<uint32_t>(l * 100000));
        auto a = spec_interleave(x, fft, l);
        ASSERT_EQ(d.symbol_index(), l);
        d.deinterleave(a.data(), out.data(), n_data);
        ASSERT_EQ(out, x) << "symbol " << l;
    }
    d.reset();
    EXPECT_EQ(d.symbol_index(), 0u);
}

// Data cell counts from Tables 7.2/7.3 style values plus the full Mmax-1
// edge; any N_data <= Mmax is legal input to the address check.
INSTANTIATE_TEST_SUITE_P(Sizes, FreqDeinterleaverSpec,
                         ::testing::Values(std::make_tuple(8192u, 6432u),
                                           std::make_tuple(8192u, 8191u),
                                           std::make_tuple(16384u, 13004u),
                                           std::make_tuple(32768u, 26304u),
                                           std::make_tuple(32768u, 27000u)));

TEST(FreqDeinterleaverTest, RejectsSizesOutsideTable711) {
    FreqDeinterleaver d({4096});
    EXPECT_FALSE(d.is_configured());
    EXPECT_TRUE(d.set_config({16384}));
    EXPECT_TRUE(d.is_configured());
}

TEST(FreqDeinterleaverTest, FirstSymbolIsNotIdentity) {
    FreqDeinterleaver d({8192});
    std::vector<uint32_t> h(6000);
    d.interleaving_sequence(0, h.size(), h.data());
    size_t fixed = 0;
    for (size_t p = 0; p < h.size(); p++) {
        fixed += h[p] == p;
    }
    EXPECT_LT(fixed, 100u);
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
