// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_l1_fec.cc — Unit tests for lib/framing/l1_fec.h
//
// Expected values: the L1-Basic cell counts of A/322 Table 6.17, and
// segmentation worked by hand from Section 6.5.3.1. The decoder is checked
// against a transmitter written here from Sections 6.5.2.2-6.5.2.10 (its
// own copies of Tables 6.20-6.24), using the library's scrambler, BCH and
// LDPC encoders, which have their own spec tests.

#include "fec/bch_decoder.h"
#include "fec/ldpc_matrix.h"
#include "framing/baseband_packet.h"
#include "framing/l1_fec.h"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

TEST(L1FecParamsTest, BasicCellCountsFollowTable617) {
    const size_t cells[7] = {3820, 934, 484, 259, 163, 112, 69};
    for (int mode = 1; mode <= 7; mode++) {
        L1FecParams p;
        ASSERT_TRUE(l1_fec_params(L1Kind::BASIC, mode, 200, &p));
        EXPECT_EQ(p.num_cells, cells[mode - 1]) << "L1-Basic Mode " << mode;
        EXPECT_EQ(p.nouter, 368u);
        EXPECT_EQ(p.rate, config::CodeRate::RATE_3_15);
    }
}

TEST(L1FecParamsTest, RejectsOutOfRangeKsig) {
    L1FecParams p;
    EXPECT_FALSE(l1_fec_params(L1Kind::BASIC, 1, 208, &p));
    EXPECT_FALSE(l1_fec_params(L1Kind::DETAIL, 1, 2360, &p));
    EXPECT_TRUE(l1_fec_params(L1Kind::DETAIL, 2, 3072, &p));
    EXPECT_FALSE(l1_fec_params(L1Kind::DETAIL, 3, 6320, &p));
    EXPECT_FALSE(l1_fec_params(L1Kind::DETAIL, 8, 400, &p));
    EXPECT_FALSE(l1_fec_params(L1Kind::DETAIL, 3, 404, &p));  // not whole bytes
}

TEST(L1FecParamsTest, DetailSegmentationExamples) {
    // Mode 3, 1000 bytes: 8000 bits over ceil(8000/6312) = 2 blocks of 4000.
    auto s = l1_detail_segmentation(3, 1000);
    EXPECT_EQ(s.num_blocks, 2u);
    EXPECT_EQ(s.padding_bits, 0u);
    EXPECT_EQ(s.ksig, 4000u);
    // Mode 1, 799 bytes: 6392 bits, 3 blocks; ceil(6392/24) * 24 = 6408,
    // so 16 padding bits and Ksig = 2136.
    s = l1_detail_segmentation(1, 799);
    EXPECT_EQ(s.num_blocks, 3u);
    EXPECT_EQ(s.padding_bits, 16u);
    EXPECT_EQ(s.ksig, 2136u);
    // 25 bytes (the minimum L1-Detail size) fits one block in every mode.
    s = l1_detail_segmentation(7, 25);
    EXPECT_EQ(s.num_blocks, 1u);
    EXPECT_EQ(s.ksig, 200u);
}

// ---- Transmitter of Section 6.5.2 ----

struct Tables {
    std::vector<int> short_order;   // Table 6.20
    std::vector<int> parity_order;  // Tables 6.21/6.22
    int a_num, a_den, b, eta;       // Table 6.24
};

Tables tables(L1Kind kind, int mode) {
    const int eta[7] = {2, 2, 2, 4, 6, 8, 8};
    if (kind == L1Kind::BASIC) {
        const int b[7] = {9360, 11460, 12360, 12292, 12350, 12432, 12776};
        return {{4, 1, 5, 2, 8, 6, 0, 7, 3},
                {20, 23, 25, 32, 38, 41, 18, 9,  10, 11, 31, 24, 14, 15, 26, 40, 33, 19,
                 28, 34, 16, 39, 27, 30, 21, 44, 43, 35, 42, 36, 12, 13, 29, 22, 37, 17},
                0, 1, b[mode - 1], eta[mode - 1]};
    }
    const std::vector<int> m46s = {0, 15, 5, 16, 17, 1, 6, 13, 11, 4, 7, 12, 8, 14, 2, 3, 9, 10};
    const std::vector<int> m46p = {20, 35, 42, 39, 26, 23, 30, 18, 28, 37, 32, 27, 44, 43,
                                   41, 40, 38, 36, 34, 33, 31, 29, 25, 24, 22, 21, 19};
    switch (mode) {
        case 1:
            return {{7, 8, 5, 4, 1, 2, 6, 3, 0},
                    {16, 22, 27, 30, 37, 44, 20, 23, 25, 32, 38, 41, 9,  10, 17, 18, 21, 33,
                     35, 14, 28, 12, 15, 19, 11, 24, 29, 34, 36, 13, 40, 43, 31, 26, 39, 42},
                    7, 2, 0, 2};
        case 2:
            return {{6, 1, 7, 8, 0, 2, 4, 3, 5},
                    {9,  31, 23, 10, 11, 25, 43, 29, 36, 16, 27, 34, 26, 18, 37, 15, 13, 17,
                     35, 21, 20, 24, 44, 12, 22, 40, 19, 32, 38, 41, 30, 33, 14, 28, 39, 42},
                    2, 1, 6036, 2};
        case 3:
            return {{0, 12, 15, 13, 2, 5, 7, 9, 8, 6, 16, 10, 14, 1, 17, 11, 4, 3},
                    {19, 37, 30, 42, 23, 44, 27, 40, 21, 34, 25, 32, 29, 24,
                     26, 35, 39, 20, 18, 43, 31, 36, 38, 22, 33, 28, 41},
                    11, 16, 4653, 2};
        case 4:
            return {m46s, m46p, 29, 32, 3200, 4};
        case 5:
            return {{2, 4, 5, 17, 9, 7, 1, 6, 15, 8, 10, 14, 16, 0, 11, 13, 12, 3},
                    {19, 37, 33, 26, 40, 43, 22, 29, 24, 35, 44, 31, 27, 20,
                     21, 39, 25, 42, 34, 18, 32, 38, 23, 30, 28, 36, 41},
                    3, 4, 4284, 6};
        case 6:
            return {m46s, m46p, 11, 16, 4900, 8};
        default:
            return {{15, 7, 8, 11, 5, 10, 16, 4, 12, 3, 0, 6, 9, 1, 14, 17, 2, 13},
                    {44, 23, 29, 33, 24, 28, 21, 27, 42, 18, 22, 31, 32, 37,
                     43, 30, 25, 35, 20, 34, 39, 36, 19, 41, 40, 26, 38},
                    49, 256, 8246, 8};
    }
}

// Returns the eta-bit cell words (y0..y(eta-1) per cell, flattened). With
// erase_main_parity, the parity bits after the repeated ones are sent as 2
// (an erasure), so only the repetition carries parity.
std::vector<uint8_t> transmit(L1Kind kind, int mode, const std::vector<uint8_t>& info,
                              bool erase_main_parity = false) {
    const Tables t = tables(kind, mode);
    const size_t ksig = info.size();
    const bool type_b = kind == L1Kind::DETAIL && mode >= 3;
    const size_t kldpc = type_b ? 6480 : 3240;
    const size_t nparity = 16200 - kldpc;
    const size_t nouter = ksig + 168;

    // 6.5.2.2 scrambling (the 5.2.3 byte scrambler).
    std::vector<uint8_t> bytes(ksig / 8);
    pack_bits_msb_first(info.data(), ksig, bytes.data());
    bbp_scramble(bytes.data(), bytes.size());
    std::vector<uint8_t> scrambled(ksig);
    for (size_t i = 0; i < ksig; i++) {
        scrambled[i] = (bytes[i / 8] >> (7 - i % 8)) & 1;
    }
    // 6.5.2.3 BCH (16K, 168 parity bits).
    fec::BchConfig bc{nouter, ksig, 12, 14};
    std::vector<uint8_t> bch(nouter);
    EXPECT_TRUE(fec::bch_encode(bc, scrambled.data(), bch.data()));
    // 6.5.2.4 zero padding, Steps 1-4.
    std::vector<int> padded(kldpc, 0);
    size_t npad = (kldpc - nouter) / 360;
    for (size_t j = 0; j < npad; j++) {
        for (size_t k = 0; k < 360; k++) {
            padded[static_cast<size_t>(t.short_order[j]) * 360 + k] = 1;
        }
    }
    if (npad < kldpc / 360) {
        for (size_t k = 0; k < kldpc - nouter - 360 * npad; k++) {
            padded[static_cast<size_t>(t.short_order[npad]) * 360 + k] = 1;
        }
    }
    std::vector<uint8_t> ldpc_info(kldpc, 0);
    size_t src = 0;
    for (size_t k = 0; k < kldpc; k++) {
        if (!padded[k]) {
            ldpc_info[k] = bch[src++];
        }
    }
    // 6.5.2.5 LDPC.
    std::vector<uint8_t> c(16200);
    EXPECT_TRUE(fec::ldpc_encode(type_b ? config::CodeRate::RATE_6_15 : config::CodeRate::RATE_3_15,
                                 true, ldpc_info.data(), c.data()));
    // 6.5.2.6 parity interleaving (Modes 3-7) and group-wise permutation.
    std::vector<uint8_t> u(c);
    if (type_b) {
        for (size_t s = 0; s < 360; s++) {
            for (size_t tt = 0; tt < 27; tt++) {
                u[kldpc + 360 * tt + s] = c[kldpc + 27 * s + tt];
            }
        }
    }
    std::vector<uint8_t> y(u);
    for (size_t j = kldpc / 360; j < 45; j++) {
        size_t from = static_cast<size_t>(t.parity_order[j - kldpc / 360]);
        for (size_t k = 0; k < 360; k++) {
            y[j * 360 + k] = u[from * 360 + k];
        }
    }
    // 6.5.2.7 repetition: [information][repeated parity][parity].
    size_t nrepeat = 0;
    if (mode == 1) {
        nrepeat = kind == L1Kind::BASIC ? 3672 : 2 * (61 * nouter / 16) - 508;
    }
    std::vector<uint8_t> word(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(kldpc));
    for (size_t q = 0; q < nrepeat; q++) {
        word.push_back(q < nparity ? y[kldpc + q] : y[kldpc + q - nparity]);
    }
    word.insert(word.end(), y.begin() + static_cast<std::ptrdiff_t>(kldpc), y.end());
    // 6.5.2.8 puncturing: drop the last Npunc bits.
    size_t npunc_temp = static_cast<size_t>(t.a_num) * (kldpc - nouter) /
                            static_cast<size_t>(t.a_den) + static_cast<size_t>(t.b);
    size_t nfec_temp = nouter + nparity - npunc_temp;
    size_t eta = static_cast<size_t>(t.eta);
    size_t nfec = (nfec_temp + eta - 1) / eta * eta;
    size_t npunc = npunc_temp - (nfec - nfec_temp);
    word.resize(word.size() - npunc);
    // 6.5.2.9 zero removal.
    std::vector<uint8_t> sent;
    for (size_t k = 0; k < word.size(); k++) {
        if (k >= kldpc || !padded[k]) {
            sent.push_back(word[k]);
        }
    }
    EXPECT_EQ(sent.size(), nfec + nrepeat);
    if (erase_main_parity) {
        for (size_t k = nouter + nrepeat; k < sent.size(); k++) {
            sent[k] = 2;
        }
    }
    // 6.5.2.10 block interleaver (column-wise write, row-wise read) and
    // bit demux (cyclic shift by row for eta > 2).
    const size_t rows = sent.size() / eta;
    std::vector<uint8_t> cells;
    for (size_t i = 0; i < rows; i++) {
        std::vector<uint8_t> b(eta);
        for (size_t col = 0; col < eta; col++) {
            b[col] = sent[col * rows + i];
        }
        for (size_t j = 0; j < eta; j++) {
            cells.push_back(eta == 2 ? b[j] : b[(i + j) % eta]);
        }
    }
    return cells;
}

class L1FecRoundTrip : public ::testing::TestWithParam<std::tuple<int, int, size_t>> {};

TEST_P(L1FecRoundTrip, DecodesTransmittedBlock) {
    auto [kind_i, mode, ksig] = GetParam();
    L1Kind kind = kind_i == 0 ? L1Kind::BASIC : L1Kind::DETAIL;
    L1FecParams p;
    ASSERT_TRUE(l1_fec_params(kind, mode, ksig, &p));
    std::mt19937 rng(static_cast<unsigned>(mode * 1000 + ksig));
    std::vector<uint8_t> info(ksig);
    for (auto& b : info) {
        b = static_cast<uint8_t>(rng() & 1);
    }
    auto cells = transmit(kind, mode, info);
    ASSERT_EQ(cells.size(), p.num_cells * p.eta);
    std::normal_distribution<float> noise(0.0f, 8.0f);
    std::vector<int8_t> llr(cells.size());
    for (size_t i = 0; i < cells.size(); i++) {
        float v = (cells[i] ? -40.0f : 40.0f) + noise(rng);
        llr[i] = static_cast<int8_t>(std::max(-127.0f, std::min(127.0f, v)));
    }
    L1FecDecoder dec;
    std::vector<uint8_t> out(ksig, 0xFF);
    L1FecResult r;
    ASSERT_TRUE(dec.decode(p, llr.data(), out.data(), &r));
    EXPECT_TRUE(r.ldpc_converged);
    EXPECT_TRUE(r.bch_ok);
    EXPECT_EQ(out, info);
}

std::string rt_name(const ::testing::TestParamInfo<std::tuple<int, int, size_t>>& info) {
    return std::string(std::get<0>(info.param) == 0 ? "basic" : "detail") + "_mode" +
           std::to_string(std::get<1>(info.param)) + "_k" + std::to_string(std::get<2>(info.param));
}

INSTANTIATE_TEST_SUITE_P(Basic, L1FecRoundTrip,
                         ::testing::Combine(::testing::Values(0), ::testing::Range(1, 8),
                                            ::testing::Values(size_t{200})),
                         rt_name);
// Mode 1 Ksig 2136 repeats more than Nldpc_parity bits (Figure 6.23).
INSTANTIATE_TEST_SUITE_P(Detail, L1FecRoundTrip,
                         ::testing::Values(std::make_tuple(1, 1, size_t{200}),
                                           std::make_tuple(1, 1, size_t{2136}),
                                           std::make_tuple(1, 2, size_t{1600}),
                                           std::make_tuple(1, 3, size_t{4000}),
                                           std::make_tuple(1, 4, size_t{6312}),
                                           std::make_tuple(1, 5, size_t{2400}),
                                           std::make_tuple(1, 6, size_t{800}),
                                           std::make_tuple(1, 7, size_t{3000})),
                         rt_name);

// L1-Basic Mode 1 repeats 3672 parity bits, more than the 3600 that survive
// puncturing; with the surviving copies erased the block still decodes only
// if each repeated bit is combined at its own parity position.
TEST(L1FecDecoderTest, RepetitionAloneCarriesParity) {
    L1FecParams p;
    ASSERT_TRUE(l1_fec_params(L1Kind::BASIC, 1, 200, &p));
    std::mt19937 rng(77);
    std::vector<uint8_t> info(200);
    for (auto& b : info) {
        b = static_cast<uint8_t>(rng() & 1);
    }
    auto cells = transmit(L1Kind::BASIC, 1, info, true);
    std::vector<int8_t> llr(cells.size());
    for (size_t i = 0; i < cells.size(); i++) {
        llr[i] = cells[i] == 2 ? 0 : (cells[i] ? -40 : 40);
    }
    L1FecDecoder dec;
    std::vector<uint8_t> out(200);
    ASSERT_TRUE(dec.decode(p, llr.data(), out.data()));
    EXPECT_EQ(out, info);
}

TEST(L1FecDecoderTest, UncorrectableBlockFails) {
    L1FecParams p;
    ASSERT_TRUE(l1_fec_params(L1Kind::BASIC, 7, 200, &p));
    std::vector<int8_t> llr(p.num_cells * p.eta);
    std::mt19937 rng(3);
    for (auto& l : llr) {
        l = static_cast<int8_t>(static_cast<int>(rng() % 255) - 127);
    }
    L1FecDecoder dec;
    std::vector<uint8_t> out(200);
    EXPECT_FALSE(dec.decode(p, llr.data(), out.data()));
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
