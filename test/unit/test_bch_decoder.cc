// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_bch_decoder.cc — Unit tests for lib/fec/bch_decoder.h
//
// Expected values come from A/322 Section 6.1.2: the Kpayload columns of
// Tables 6.1/6.2, and g(x) as the product of the Table 6.3 polynomials as
// printed (the library derives g(x) from the field instead). The CRC-32
// reference is polynomial long division, not the shift register.

#include "bch_decoder.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace fec {
namespace {

using config::CodeRate;

// Table 6.3, exponents of each g_i(x) (the "+1" term is exponent 0).
const std::vector<std::vector<int>> kTable63Long = {
    {16, 5, 3, 2, 0},
    {16, 8, 6, 5, 4, 1, 0},
    {16, 11, 10, 9, 8, 7, 5, 4, 3, 2, 0},
    {16, 14, 12, 11, 9, 6, 4, 2, 0},
    {16, 12, 11, 10, 9, 8, 5, 3, 2, 1, 0},
    {16, 15, 14, 13, 12, 10, 9, 8, 7, 5, 4, 2, 0},
    {16, 15, 13, 11, 10, 9, 8, 6, 5, 2, 0},
    {16, 14, 13, 12, 9, 8, 6, 5, 2, 1, 0},
    {16, 11, 10, 9, 7, 5, 0},
    {16, 14, 13, 12, 10, 8, 7, 5, 2, 1, 0},
    {16, 13, 12, 11, 9, 5, 3, 2, 0},
    {16, 12, 11, 9, 7, 6, 5, 1, 0},
};
const std::vector<std::vector<int>> kTable63Short = {
    {14, 5, 3, 1, 0},
    {14, 11, 8, 6, 0},
    {14, 10, 9, 6, 2, 1, 0},
    {14, 12, 10, 8, 7, 4, 0},
    {14, 13, 11, 9, 8, 6, 4, 2, 0},
    {14, 13, 9, 8, 7, 3, 0},
    {14, 13, 11, 10, 7, 6, 5, 2, 0},
    {14, 11, 10, 9, 8, 5, 0},
    {14, 10, 9, 3, 2, 1, 0},
    {14, 12, 11, 9, 6, 3, 0},
    {14, 12, 11, 4, 0},
    {14, 13, 10, 8, 7, 6, 5, 3, 2, 1, 0},
};

// Kpayload (BCH) columns of Tables 6.1 and 6.2, rates 2/15..13/15.
const size_t kKpayloadLong[12] = {8448,  12768, 17088, 21408, 25728, 30048,
                                  34368, 38688, 43008, 47328, 51648, 55968};
const size_t kKpayloadShort[12] = {1992, 3072, 4152,  5232,  6312,  7392,
                                   8472, 9552, 10632, 11712, 12792, 13872};

// g(x) coefficients, lowest power first.
std::vector<uint8_t> table63_generator(bool short_frame) {
    std::vector<uint8_t> g{1};
    for (const auto& exps : short_frame ? kTable63Short : kTable63Long) {
        std::vector<uint8_t> gi(static_cast<size_t>(exps.front()) + 1, 0);
        for (int e : exps) {
            gi[static_cast<size_t>(e)] = 1;
        }
        std::vector<uint8_t> prod(g.size() + gi.size() - 1, 0);
        for (size_t a = 0; a < g.size(); a++) {
            for (size_t b = 0; b < gi.size(); b++) {
                prod[a + b] ^= static_cast<uint8_t>(g[a] & gi[b]);
            }
        }
        g.swap(prod);
    }
    return g;
}

// s(x) = m(x) x^Mouter - (m(x) x^Mouter mod g(x)) by long division, with
// bit i the coefficient of x^(N-1-i).
std::vector<uint8_t> reference_encode(const std::vector<uint8_t>& payload, bool short_frame) {
    std::vector<uint8_t> g = table63_generator(short_frame);
    size_t mouter = g.size() - 1;
    std::vector<uint8_t> cw(payload);
    cw.resize(payload.size() + mouter, 0);
    std::vector<uint8_t> rem(cw);
    for (size_t i = 0; i < payload.size(); i++) {
        if (rem[i]) {
            for (size_t k = 0; k <= mouter; k++) {
                rem[i + k] ^= g[mouter - k];
            }
        }
    }
    for (size_t k = 0; k < mouter; k++) {
        cw[payload.size() + k] = rem[payload.size() + k];
    }
    return cw;
}

std::vector<uint8_t> random_bits(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) {
        b = static_cast<uint8_t>(rng() & 1);
    }
    return v;
}

void flip_random(std::vector<uint8_t>& cw, size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<size_t> idx(cw.size());
    for (size_t i = 0; i < idx.size(); i++) {
        idx[i] = i;
    }
    std::shuffle(idx.begin(), idx.end(), rng);
    for (size_t i = 0; i < count; i++) {
        cw[idx[i]] ^= 1;
    }
}

TEST(BchTable63, GeneratorDegreeIsMouter) {
    EXPECT_EQ(table63_generator(false).size() - 1, 192u);
    EXPECT_EQ(table63_generator(true).size() - 1, 168u);
}

TEST(BchConfigTest, LengthsFollowTables61And62) {
    for (int r = 0; r < 12; r++) {
        auto rate = static_cast<CodeRate>(r);
        BchConfig lc = get_atsc3_bch_config(rate, false);
        EXPECT_EQ(lc.info_length, kKpayloadLong[r]) << "rate " << r + 2 << "/15";
        EXPECT_EQ(lc.codeword_length, kKpayloadLong[r] + 192);
        EXPECT_EQ(lc.m, 16u);
        BchConfig sc = get_atsc3_bch_config(rate, true);
        EXPECT_EQ(sc.info_length, kKpayloadShort[r]) << "rate " << r + 2 << "/15";
        EXPECT_EQ(sc.codeword_length, kKpayloadShort[r] + 168);
        EXPECT_EQ(sc.m, 14u);
        EXPECT_EQ(sc.t, 12u);
    }
}

TEST(BchConfigTest, NonSpecConfigIsRejected) {
    BchConfig c = get_atsc3_bch_config(CodeRate::RATE_8_15, false);
    c.m = 15;
    BchDecoder dec(c);
    EXPECT_FALSE(dec.is_initialized());
    std::vector<uint8_t> cw(c.codeword_length, 0);
    EXPECT_FALSE(dec.decode(cw.data(), cw.size()).success);
    std::vector<uint8_t> payload(c.info_length, 0);
    EXPECT_FALSE(bch_encode(c, payload.data(), cw.data()));
}

class BchCodeTest : public ::testing::TestWithParam<std::tuple<int, bool>> {
protected:
    CodeRate rate() const {
        return static_cast<CodeRate>(std::get<0>(GetParam()));
    }
    bool short_frame() const {
        return std::get<1>(GetParam());
    }
};

TEST_P(BchCodeTest, EncoderMatchesTable63Product) {
    BchConfig c = get_atsc3_bch_config(rate(), short_frame());
    auto payload = random_bits(c.info_length, 7);
    std::vector<uint8_t> cw(c.codeword_length);
    ASSERT_TRUE(bch_encode(c, payload.data(), cw.data()));
    EXPECT_EQ(cw, reference_encode(payload, short_frame()));
}

TEST_P(BchCodeTest, ValidCodewordHasZeroSyndromes) {
    BchConfig c = get_atsc3_bch_config(rate(), short_frame());
    auto payload = random_bits(c.info_length, 11);
    auto cw = reference_encode(payload, short_frame());
    BchDecoder dec(c);
    BchResult r = dec.decode(cw.data(), cw.size());
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.was_valid);
    EXPECT_EQ(r.errors_corrected, 0u);
    EXPECT_EQ(r.decoded_bits, payload);
}

TEST_P(BchCodeTest, CorrectsUpToTwelveErrors) {
    BchConfig c = get_atsc3_bch_config(rate(), short_frame());
    auto payload = random_bits(c.info_length, 13);
    auto clean = reference_encode(payload, short_frame());
    BchDecoder dec(c);
    for (size_t n : {1u, 5u, 12u}) {
        auto cw = clean;
        flip_random(cw, n, static_cast<unsigned>(100 + n));
        BchResult r = dec.decode(cw.data(), cw.size());
        ASSERT_TRUE(r.success) << n << " errors";
        EXPECT_FALSE(r.was_valid);
        EXPECT_EQ(r.errors_corrected, n);
        EXPECT_EQ(r.decoded_bits, payload) << n << " errors";
    }
}

TEST_P(BchCodeTest, ThirteenErrorsAreNotCorrected) {
    BchConfig c = get_atsc3_bch_config(rate(), short_frame());
    auto payload = random_bits(c.info_length, 17);
    auto cw = reference_encode(payload, short_frame());
    flip_random(cw, 13, 1300);
    BchDecoder dec(c);
    EXPECT_FALSE(dec.decode(cw.data(), cw.size()).success);
}

std::string code_name(const ::testing::TestParamInfo<std::tuple<int, bool>>& info) {
    return "rate" + std::to_string(std::get<0>(info.param) + 2) + "_15_" +
           (std::get<1>(info.param) ? "short" : "long");
}

// Smallest and largest codes of each length, plus a mid rate.
INSTANTIATE_TEST_SUITE_P(Codes, BchCodeTest,
                         ::testing::Combine(::testing::Values(0, 5, 11),
                                            ::testing::Values(false, true)),
                         code_name);

TEST(BchDecoderTest, ErrorsAtBothEndsAndInParity) {
    BchConfig c = get_atsc3_bch_config(CodeRate::RATE_2_15, true);
    auto payload = random_bits(c.info_length, 19);
    auto cw = reference_encode(payload, true);
    for (size_t pos : {size_t{0}, c.info_length - 1, c.info_length, c.codeword_length - 1}) {
        cw[pos] ^= 1;
    }
    BchDecoder dec(c);
    BchResult r = dec.decode(cw.data(), cw.size());
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.errors_corrected, 4u);
    EXPECT_EQ(r.decoded_bits, payload);
}

TEST(BchDecoderTest, PreallocatedOutputAndLengthCheck) {
    BchConfig c = get_atsc3_bch_config(CodeRate::RATE_6_15, true);
    auto payload = random_bits(c.info_length, 23);
    auto cw = reference_encode(payload, true);
    cw[42] ^= 1;
    BchDecoder dec(c);
    std::vector<uint8_t> out(c.info_length, 0xFF);
    EXPECT_EQ(dec.decode(cw.data(), cw.size(), out.data()), 1);
    EXPECT_EQ(out, payload);
    EXPECT_EQ(dec.decode(cw.data(), cw.size() - 1, out.data()), -1);
    EXPECT_EQ(dec.decode(nullptr, cw.size(), out.data()), -1);
    EXPECT_EQ(dec.decode(cw.data(), cw.size(), nullptr), -1);
}

TEST(BchDecoderTest, SetConfigSwitchesField) {
    BchDecoder dec(get_atsc3_bch_config(CodeRate::RATE_13_15, false));
    BchConfig c = get_atsc3_bch_config(CodeRate::RATE_3_15, true);
    dec.set_config(c);
    ASSERT_TRUE(dec.is_initialized());
    EXPECT_EQ(dec.parity_bits(), 168u);
    auto payload = random_bits(c.info_length, 29);
    auto cw = reference_encode(payload, true);
    flip_random(cw, 12, 31);
    BchResult r = dec.decode(cw.data(), cw.size());
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.decoded_bits, payload);
}

// CRC-32 reference: M(x) with its first 32 bits inverted (the all-ones
// initial register), times x^32, mod G(x) = x^32+x^21+x^16+x^11+1.
uint32_t reference_crc32(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> d(bits);
    for (size_t i = 0; i < 32 && i < d.size(); i++) {
        d[i] ^= 1;
    }
    d.resize(d.size() + 32, 0);
    const int exps[] = {32, 21, 16, 11, 0};
    for (size_t i = 0; i + 32 < d.size(); i++) {
        if (d[i]) {
            for (int e : exps) {
                d[i + 32 - static_cast<size_t>(e)] ^= 1;
            }
        }
    }
    uint32_t crc = 0;
    for (size_t k = d.size() - 32; k < d.size(); k++) {
        crc = (crc << 1) | d[k];
    }
    return crc;
}

TEST(OuterCrc32Test, MatchesPolynomialDivision) {
    for (size_t n : {32u, 33u, 100u, 2128u}) {
        auto bits = random_bits(n, static_cast<unsigned>(n));
        EXPECT_EQ(outer_crc32(bits.data(), n), reference_crc32(bits)) << n << " bits";
    }
}

TEST(OuterCrc32Test, MessageWithAppendedCrcHasZeroRemainder) {
    auto bits = random_bits(1000, 3);
    uint32_t crc = outer_crc32(bits.data(), bits.size());
    for (int b = 31; b >= 0; b--) {
        bits.push_back(static_cast<uint8_t>((crc >> b) & 1));
    }
    EXPECT_EQ(outer_crc32(bits.data(), bits.size()), 0u);
}

}  // namespace
}  // namespace fec
}  // namespace atsc3
