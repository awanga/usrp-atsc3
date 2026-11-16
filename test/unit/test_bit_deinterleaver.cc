// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_bit_deinterleaver.cc — Unit tests for lib/fec/bit_deinterleaver.h
//
// The reference is a transmitter-side model written from A/322 Section 6.2
// as printed: the parity interleaver formula, Y_j = X_pi(j) with pi from the
// Annex B tables, and the block interleavers with the row/part sizes typed
// in from Tables 6.10 and 6.11 (the library derives them). Spec examples
// (the 256QAM Type A output order and Figures 6.8/6.9) are checked directly.

#include "bit_deinterleaver.h"
#include "bit_interleaver_tables.h"
#include "ldpc_decoder.h"
#include "ldpc_matrix.h"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace fec {
namespace {

using config::CodeRate;
using config::Modulation;

const Modulation kMods[6] = {Modulation::QPSK,   Modulation::NUC_16,   Modulation::NUC_64,
                             Modulation::NUC_256, Modulation::NUC_1024, Modulation::NUC_4096};

// Table 6.10 (Type A): {Nr1, Nr2} per modulation, 64800 then 16200.
const size_t kNr1Long[6] = {32400, 16200, 10800, 7920, 6480, 5400};
const size_t kNr2Long[6] = {0, 0, 0, 180, 0, 0};
const size_t kNr1Short[4] = {7920, 3960, 2520, 1800};
const size_t kNr2Short[4] = {180, 90, 180, 225};
// Table 6.11 (Type B): Npart1 per modulation.
const size_t kNpart1Long[6] = {64800, 64800, 64800, 63360, 64800, 64800};
const size_t kNpart1Short[4] = {15840, 15840, 15120, 14400};

// Tables 6.8/6.9 rows typed in as printed (eta = 2, 4, ...).
const char* const kTable68[6] = {"AAAAAAAAAAAA", "AAABAABBAAAA", "AAAAABABBAAB",
                                 "AAABBBBABBAB", "AAABABABBBAA", "AAAAABAAAAAA"};
const char* const kTable69[4] = {"AAAABBABAAAA", "AAAABBABABAB", "AAAABBABAAAA",
                                 "AAAABAAAAABA"};

// Transmitted bit stream for codeword lambda (A/322 6.2.1 - 6.3.3).
template <typename T>
std::vector<T> transmit(const std::vector<T>& lambda, CodeRate rate, int mod, bool short_cw) {
    const size_t ninner = short_cw ? 16200 : 64800;
    const size_t numer = static_cast<size_t>(rate) + 2;
    const size_t nouter = ninner * numer / 15;
    const size_t qldpc = (ninner - nouter) / 360;
    const size_t ngroup = ninner / 360;
    const size_t eta = 2 * static_cast<size_t>(mod) + 2;
    // Type B LDPC codes (Table 6.4).
    const bool type_b_ldpc = numer >= 6 && !(numer == 7 && !short_cw);

    std::vector<T> u(lambda);
    if (type_b_ldpc) {
        for (size_t s = 0; s < 360; s++) {
            for (size_t t = 0; t < qldpc; t++) {
                u[nouter + 360 * t + s] = lambda[nouter + qldpc * s + t];
            }
        }
    }

    const uint8_t* pi = short_cw ? k_group_order_short[mod][static_cast<size_t>(rate)]
                                 : k_group_order_long[mod][static_cast<size_t>(rate)];
    std::vector<T> v(ninner);
    for (size_t j = 0; j < ngroup; j++) {
        for (size_t b = 0; b < 360; b++) {
            v[360 * j + b] = u[360 * pi[j] + b];
        }
    }

    std::vector<T> q(ninner);
    const char type = (short_cw ? kTable69 : kTable68)[mod][static_cast<size_t>(rate)];
    if (type == 'A') {
        const size_t nr1 = short_cw ? kNr1Short[mod] : kNr1Long[mod];
        const size_t nr2 = short_cw ? kNr2Short[mod] : kNr2Long[mod];
        const size_t nc = eta;
        std::vector<std::vector<T>> grid(nr1 + nr2, std::vector<T>(nc));
        for (size_t i = 0; i < ninner; i++) {
            if (i < nc * nr1) {
                grid[i % nr1][i / nr1] = v[i];
            } else {
                grid[nr1 + (i - nc * nr1) % nr2][(i - nc * nr1) / nr2] = v[i];
            }
        }
        for (size_t j = 0; j < ninner; j++) {
            q[j] = grid[j / nc][j % nc];
        }
    } else {
        const size_t npart1 = short_cw ? kNpart1Short[mod] : kNpart1Long[mod];
        size_t out = 0;
        for (size_t base = 0; base < npart1; base += eta * 360) {
            for (size_t col = 0; col < 360; col++) {
                for (size_t row = 0; row < eta; row++) {
                    q[out++] = v[base + row * 360 + col];
                }
            }
        }
        for (size_t i = npart1; i < ninner; i++) {
            q[out++] = v[i];
        }
    }
    return q;  // demultiplexer: cell s = q[eta*s .. eta*s + eta-1]
}

TEST(BlockInterleaverTypeTest, MatchesTables68And69) {
    for (int m = 0; m < 6; m++) {
        for (int r = 0; r < 12; r++) {
            size_t eta = 2 * static_cast<size_t>(m) + 2;
            auto rate = static_cast<CodeRate>(r);
            EXPECT_EQ(block_interleaver_type(rate, eta, false) == BlockInterleaverType::TYPE_A,
                      kTable68[m][r] == 'A')
                << "eta " << eta << " rate " << r + 2;
            if (m < 4) {
                EXPECT_EQ(block_interleaver_type(rate, eta, true) == BlockInterleaverType::TYPE_A,
                          kTable69[m][r] == 'A')
                    << "eta " << eta << " rate " << r + 2 << " short";
            }
        }
    }
}

TEST(BitDeinterleaverTest, UndefinedCombinationsAreRejected) {
    BitDeinterleaver d;
    EXPECT_FALSE(d.configure(CodeRate::RATE_8_15, Modulation::NUC_1024, true));
    EXPECT_FALSE(d.is_configured());
    EXPECT_FALSE(d.configure(CodeRate::RATE_8_15, Modulation::NUC_4096, true));
    EXPECT_TRUE(d.configure(CodeRate::RATE_8_15, Modulation::NUC_4096, false));
    EXPECT_EQ(d.codeword_length(), 64800u);
}

// Section 6.2.3.1 example: 256QAM, Ninner = 64800, Type A block interleaver
// (rate 2/15, Table 6.8): q = (v0, v7920, v15840, ..., v55440, v1, v7921, ...,
// v63359, v63360, v63540, ...). Rate 2/15 is a Type A LDPC code, so lambda
// reaches v through the group-wise interleaver only.
TEST(BitDeinterleaverTest, Type256QamExampleOrder) {
    BitDeinterleaver d;
    ASSERT_TRUE(d.configure(CodeRate::RATE_2_15, Modulation::NUC_256, false));
    const uint8_t* pi = k_group_order_long[3][0];
    auto lambda_of_v = [&](size_t i) { return 360 * size_t{pi[i / 360]} + i % 360; };
    const size_t expect[][2] = {
        {0, 0},         {1, 7920},      {2, 15840},     {7, 55440},     {8, 1},
        {9, 7921},      {63352, 7919},  {63358, 55439}, {63359, 63359}, {63360, 63360},
        {63361, 63540}, {63367, 64620}, {64799, 64799},
    };
    for (const auto& e : expect) {
        EXPECT_EQ(d.source_index(lambda_of_v(e[1])), e[0]) << "q" << e[0] << " = v" << e[1];
    }
}

// Figures 6.8/6.9: Type B, 256QAM (rate 6/15 long, Table 6.8) reads
// v0, v360, ..., v2520, then v1, v361, ...
TEST(BitDeinterleaverTest, TypeBFigure69Order) {
    BitDeinterleaver d;
    ASSERT_TRUE(d.configure(CodeRate::RATE_6_15, Modulation::NUC_256, false));
    const uint8_t* pi = k_group_order_long[3][4];
    // 6/15 is a Type B LDPC code; group pi[j] < Nouter/360 holds info bits,
    // which the parity interleaver leaves in place.
    auto lambda_of_v = [&](size_t i) {
        size_t u = 360 * size_t{pi[i / 360]} + i % 360;
        EXPECT_LT(u, 25920u);
        return u;
    };
    for (size_t row = 0; row < 8; row++) {
        if (pi[row] < 72) {
            EXPECT_EQ(d.source_index(lambda_of_v(360 * row)), row);
            EXPECT_EQ(d.source_index(lambda_of_v(360 * row + 1)), 8 + row);
        }
    }
}

class BitDeinterleaverRoundTrip : public ::testing::TestWithParam<std::tuple<int, int, bool>> {};

TEST_P(BitDeinterleaverRoundTrip, InvertsTransmitterModel) {
    auto [mod, r, short_cw] = GetParam();
    auto rate = static_cast<CodeRate>(r);
    BitDeinterleaver d;
    ASSERT_TRUE(d.configure(rate, kMods[mod], short_cw));
    const size_t n = d.codeword_length();
    std::vector<uint32_t> lambda(n);
    for (size_t k = 0; k < n; k++) {
        lambda[k] = static_cast<uint32_t>(k);
    }
    std::vector<uint32_t> tx = transmit(lambda, rate, mod, short_cw);
    for (size_t k = 0; k < n; k++) {
        ASSERT_EQ(tx[d.source_index(k)], k) << "codeword bit " << k;
    }
}

std::string combo_name(const ::testing::TestParamInfo<std::tuple<int, int, bool>>& info) {
    static const char* names[] = {"qpsk", "16qam", "64qam", "256qam", "1024qam", "4096qam"};
    return std::string(names[std::get<0>(info.param)]) + "_r" +
           std::to_string(std::get<1>(info.param) + 2) + (std::get<2>(info.param) ? "_short" : "_long");
}

INSTANTIATE_TEST_SUITE_P(Long, BitDeinterleaverRoundTrip,
                         ::testing::Combine(::testing::Range(0, 6), ::testing::Range(0, 12),
                                            ::testing::Values(false)),
                         combo_name);
INSTANTIATE_TEST_SUITE_P(Short, BitDeinterleaverRoundTrip,
                         ::testing::Combine(::testing::Range(0, 4), ::testing::Range(0, 12),
                                            ::testing::Values(true)),
                         combo_name);

// Encoded codeword -> transmitter interleaving -> LLRs -> de-interleave ->
// LDPC decode recovers the codeword: the output order is H's column order.
TEST(BitDeinterleaverTest, DeinterleavedLlrsDecode) {
    const CodeRate rate = CodeRate::RATE_10_15;  // Type B LDPC, Type B block (64QAM short)
    BitDeinterleaver d;
    ASSERT_TRUE(d.configure(rate, Modulation::NUC_64, true));
    LdpcConfig cfg;
    cfg.code_rate = rate;
    cfg.short_codeword = true;
    LdpcDecoder dec(cfg);
    std::mt19937 rng(5);
    std::vector<uint8_t> info(dec.info_bits()), cw(16200);
    for (auto& b : info) {
        b = static_cast<uint8_t>(rng() & 1);
    }
    ASSERT_TRUE(ldpc_encode(rate, true, info.data(), cw.data()));
    std::vector<int8_t> llr_cw(16200);
    for (size_t k = 0; k < cw.size(); k++) {
        llr_cw[k] = cw[k] ? -20 : 20;
    }
    std::vector<int8_t> tx = transmit(llr_cw, rate, 2, true);
    std::normal_distribution<float> noise(0.0f, 6.0f);
    for (auto& l : tx) {
        l = static_cast<int8_t>(std::max(-127.0f, std::min(127.0f, l + noise(rng))));
    }
    std::vector<int8_t> rx(16200);
    d.deinterleave(tx.data(), rx.data());
    LdpcResult res = dec.decode(rx.data());
    ASSERT_TRUE(res.converged);
    EXPECT_EQ(res.decoded_bits, info);
}

}  // namespace
}  // namespace fec
}  // namespace atsc3
