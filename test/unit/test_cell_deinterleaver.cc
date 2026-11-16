// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_cell_deinterleaver.cc — Unit tests for lib/ofdm/cell_deinterleaver.h
//
// The reference is the A/322 Section 7.1.5.2 Cell Interleaver written out
// from the text (bit-array S_i register, toggle bit, address check, P(r)
// loop over a bit-reversed counter); the P(r) values of the section's
// worked example are checked directly.

#include "ofdm/cell_deinterleaver.h"

#include <gtest/gtest.h>
#include <numeric>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

std::vector<uint32_t> spec_l0(size_t ncells) {
    int nd = 0;
    while ((size_t{1} << nd) < ncells) {
        nd++;
    }
    std::vector<int> taps;
    switch (nd) {
        case 11: taps = {0, 3}; break;
        case 12: taps = {0, 2}; break;
        case 13: taps = {0, 1, 4, 6}; break;
        case 14: taps = {0, 1, 4, 5, 9, 11}; break;
        default: taps = {0, 1, 2, 12}; break;
    }
    std::vector<int> s(static_cast<size_t>(nd), 0);
    std::vector<uint32_t> l0;
    for (size_t i = 0; i < (size_t{1} << nd); i++) {
        if (i == 2) {
            s[0] = 1;
        } else if (i > 2) {
            std::vector<int> next(s);
            for (int j = 0; j <= nd - 3; j++) {
                next[static_cast<size_t>(j)] = s[static_cast<size_t>(j + 1)];
            }
            int fb = 0;
            for (int t : taps) {
                fb ^= s[static_cast<size_t>(t)];
            }
            next[static_cast<size_t>(nd - 2)] = fb;
            s = next;
        }
        s[static_cast<size_t>(nd - 1)] = static_cast<int>(i % 2);
        uint32_t v = 0;
        for (int j = 0; j < nd; j++) {
            v += static_cast<uint32_t>(s[static_cast<size_t>(j)]) << j;
        }
        if (v < ncells) {
            l0.push_back(v);
        }
    }
    return l0;
}

std::vector<size_t> spec_shifts(size_t ncells, size_t count) {
    int nd = 0;
    while ((size_t{1} << nd) < ncells) {
        nd++;
    }
    std::vector<size_t> p;
    size_t k = 0;
    for (size_t r = 0; r < count; r++) {
        size_t v = ncells;
        while (v >= ncells) {
            v = 0;
            for (int j = 0; j < nd; j++) {
                size_t bit = (k >> j) & 1;  // floor(k / 2^j) mod 2
                v += bit << (nd - 1 - j);
            }
            k++;
        }
        p.push_back(v);
    }
    return p;
}

TEST(CellDeinterleaverTest, ShiftExampleOfSection7152) {
    CellDeinterleaver d({10800});
    ASSERT_TRUE(d.is_configured());
    const size_t expect[] = {0, 8192, 4096, 2048, 10240, 6144, 1024, 9216};
    for (size_t r = 0; r < 8; r++) {
        EXPECT_EQ(d.shift(r), expect[r]) << "P(" << r << ")";
    }
}

class CellDeinterleaverSizes : public ::testing::TestWithParam<size_t> {};

// Every FEC Block size of Table 6.14.
INSTANTIATE_TEST_SUITE_P(Table614, CellDeinterleaverSizes,
                         ::testing::Values(32400u, 16200u, 10800u, 8100u, 6480u, 5400u, 4050u,
                                           2700u, 2025u));

TEST_P(CellDeinterleaverSizes, BasicPermutationAndShiftsMatchSpec) {
    const size_t n = GetParam();
    CellDeinterleaver d({n});
    ASSERT_TRUE(d.is_configured());
    auto l0 = spec_l0(n);
    ASSERT_EQ(l0.size(), n);
    for (size_t q = 0; q < n; q++) {
        ASSERT_EQ(d.basic_permutation(q), l0[q]) << "L0(" << q << ")";
    }
    auto p = spec_shifts(n, 20);
    for (size_t r = 0; r < p.size(); r++) {
        EXPECT_EQ(d.shift(r), p[r]) << "P(" << r << ")";
    }
}

TEST_P(CellDeinterleaverSizes, InvertsSpecInterleaver) {
    const size_t n = GetParam();
    CellDeinterleaver d({n});
    auto l0 = spec_l0(n);
    auto p = spec_shifts(n, 4);
    std::vector<uint32_t> g(n), dcells(n), out(n);
    for (size_t r = 0; r < 4; r++) {
        std::iota(g.begin(), g.end(), static_cast<uint32_t>(r * n));
        for (size_t q = 0; q < n; q++) {
            dcells[q] = g[(l0[q] + p[r]) % n];  // d_{r,q} = g_{r,L_r(q)}
        }
        d.deinterleave(dcells.data(), out.data(), r);
        ASSERT_EQ(out, g) << "FEC Block " << r;
    }
}

TEST(CellDeinterleaverTest, RejectsSizesOutsideNdRange) {
    CellDeinterleaver d({1000});
    EXPECT_FALSE(d.is_configured());
    EXPECT_FALSE(d.set_config({40000}));
    EXPECT_TRUE(d.set_config({2025}));
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
