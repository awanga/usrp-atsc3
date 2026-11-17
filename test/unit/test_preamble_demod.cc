// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_preamble_demod.cc — Unit tests for lib/framing/preamble_demod.h
//
// The transmitter is built here from the spec: pilots 2 A (1/2 - r_k) with
// r_k from a test-side Section 8.1.2 generator and A from Tables 8.6 /
// 8.5, data cells frequency-interleaved with the Section 7.3 sequence
// (FreqDeinterleaver::interleaving_sequence, verified in its own tests),
// carriers on DC-centred bins at N/2 - (NoC - 1)/2 + k (Section 8.3),
// through a smooth frequency-selective channel. Cell counts are Table 7.2.

#include "framing/preamble_demod.h"

#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

using cd = std::complex<double>;

// Section 8.1.2: initial state 0000 0000 1101 1 read out first, then
// s[n] = s[n-9] ^ s[n-10] ^ s[n-12] ^ s[n-13] (G = 1 + x^9 + x^10 + x^12 + x^13).
std::vector<int> ref_seq(size_t n) {
    const int init[13] = {1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<int> s(n);
    for (size_t i = 0; i < n; ++i) {
        s[i] = i < 13 ? init[i] : (s[i - 9] ^ s[i - 10] ^ s[i - 12] ^ s[i - 13]);
    }
    return s;
}

sample_t q(cd v) {
#ifdef ATSC3_FIXED_POINT
    return sample_t(float_to_q15(static_cast<float>(v.real())),
                    float_to_q15(static_cast<float>(v.imag())));
#else
    return sample_t(static_cast<float>(v.real()), static_cast<float>(v.imag()));
#endif
}

cd to_cd(sample_t s) {
#ifdef ATSC3_FIXED_POINT
    return cd(q15_to_float(s.real()), q15_to_float(s.imag()));
#else
    return cd(s.real(), s.imag());
#endif
}

struct TxSymbol {
    std::vector<sample_t> bins;
    std::vector<cd> cells;  // before interleaving
};

// One preamble symbol: 8K, Dx 16 (5.30 dB pilots), carrier reduction cred.
TxSymbol transmit(size_t l, unsigned cred, std::mt19937& rng) {
    const size_t n = 8192, dx = 16;
    ofdm::PreambleLayout lay;
    EXPECT_TRUE(ofdm::preamble_layout(n, dx, cred, &lay));
    const size_t noc = lay.noc;
    const size_t first = n / 2 - (noc - 1) / 2;
    const std::vector<int> r = ref_seq(noc);
    const double a_p = std::pow(10.0, 5.30 / 20.0), a_cp = std::pow(10.0, 8.52 / 20.0);

    TxSymbol t;
    const double s = 1.0 / std::sqrt(2.0);
    std::uniform_int_distribution<int> bit(0, 1);
    for (size_t i = 0; i < lay.data.size(); ++i) {
        t.cells.emplace_back(bit(rng) ? s : -s, bit(rng) ? s : -s);
    }
    std::vector<uint32_t> h(lay.data.size());
    ofdm::FreqDeinterleaver fi;
    fi.interleaving_sequence(l, lay.data.size(), h.data());
    std::vector<cd> freq(noc, 0.0);
    for (size_t p = 0; p < lay.data.size(); ++p) {
        freq[lay.data[p]] = t.cells[h[p]];  // a[p] = x[H_l(p)] (8K)
    }
    for (uint32_t k : lay.pilots) {
        freq[k] = 2.0 * a_p * (0.5 - r[k]);
    }
    for (uint32_t k : lay.continual) {
        freq[k] = 2.0 * a_cp * (0.5 - r[k]);
    }
    t.bins.assign(n, sample_t{});
    for (size_t k = 0; k < noc; ++k) {
        // Two paths 3 samples apart: smooth across the 16-carrier pilot gaps.
        cd ch = 0.3 * std::exp(cd(0.0, 0.7)) *
                (1.0 + 0.3 * std::exp(cd(0.0, -2.0 * M_PI * 3.0 * k / n)));
        t.bins[first + k] = q(ch * freq[k]);
    }
    return t;
}

TEST(PreambleDemodTest, RecoversCellsInTransmittedOrder) {
    PreambleDemodConfig cfg;
    cfg.fft_size = 8192;
    cfg.dx = 16;
    cfg.later_cred = 0;
    PreambleDemodulator demod(cfg);
    std::mt19937 rng(42);
    for (size_t l = 0; l < 3; ++l) {
        TxSymbol t = transmit(l, l == 0 ? 4 : 0, rng);
        // Table 7.2, 8K / Dx 16: 6075 cells at Cred 4, 6432 at Cred 0.
        ASSERT_EQ(demod.num_cells(), l == 0 ? 6075u : 6432u);
        std::vector<sample_t> cells(demod.num_cells());
        ASSERT_EQ(demod.demodulate(t.bins.data(), cells.data()), t.cells.size());
        double worst = 0.0;
        for (size_t i = 0; i < cells.size(); ++i) {
            worst = std::max(worst, std::abs(to_cd(cells[i]) - t.cells[i]));
        }
#ifdef ATSC3_FIXED_POINT
        EXPECT_LT(worst, 0.05) << "symbol " << l;
#else
        EXPECT_LT(worst, 0.01) << "symbol " << l;
#endif
    }
}

TEST(PreambleDemodTest, ResetRestartsAtTheFirstSymbol) {
    PreambleDemodulator demod(PreambleDemodConfig{});
    std::mt19937 rng(7);
    TxSymbol t0 = transmit(0, 4, rng);
    std::vector<sample_t> cells(demod.num_cells());
    demod.demodulate(t0.bins.data(), cells.data());
    EXPECT_EQ(demod.symbol_index(), 1u);
    demod.reset();
    EXPECT_EQ(demod.num_cells(), 6075u);
    demod.demodulate(t0.bins.data(), cells.data());
    EXPECT_LT(std::abs(to_cd(cells[100]) - t0.cells[100]), 0.05);
}

TEST(PreambleDemodTest, RejectsPairsOutsideTableH11) {
    PreambleDemodConfig cfg;
    cfg.dx = 32;  // not used with 8K
    EXPECT_THROW(PreambleDemodulator d(cfg), std::invalid_argument);
    cfg = PreambleDemodConfig{};
    cfg.later_cred = 5;
    EXPECT_THROW(PreambleDemodulator d(cfg), std::invalid_argument);
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
