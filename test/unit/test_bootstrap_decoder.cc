// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_bootstrap_decoder.cc — Unit tests for lib/sync/bootstrap_decoder.h
//
// The input is the test-side bootstrap transmitter (bootstrap_tx_model.h,
// written from A/321 Sections 5.2-5.4); the expected values are the fields
// it was given, the timing and frequency offsets the test applied, and the
// A/321 rules for termination and versions. Float and fixed-point builds
// run the same cases against the same expected values, including the
// noisy and multipath ones.

#include "bootstrap_tx_model.h"
#include "sync/bootstrap_decoder.h"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

using test::BootstrapTxModel;
using cd = std::complex<double>;

constexpr double kScale = 0.15;  // keeps the Q1.15 build clear of clipping
constexpr size_t kPrefix = 1000;

std::vector<cd> make_bootstrap(const std::array<int, 3>& fields, uint16_t seed = 0x019D,
                               int extra_symbols = 0) {
    std::vector<std::vector<int>> bits = BootstrapTxModel::fields_to_bits(fields);
    for (int i = 0; i < extra_symbols; i++) {
        bits.push_back(std::vector<int>(8, 0));
    }
    return BootstrapTxModel::waveform(137, seed, bits);
}

struct Channel {
    double snr_db = 200.0;
    double cfo_bins = 0.0;  // in units of the 3 kHz spacing
    std::vector<cd> taps{1.0};
    unsigned seed = 1;
};

// kPrefix samples of noise-only lead-in, then the bootstrap, then a tail.
std::vector<sample_t> receive(const std::vector<cd>& tx, const Channel& ch) {
    std::vector<cd> x(kPrefix + tx.size() + 2000, 0.0);
    for (size_t i = 0; i < tx.size(); i++) {
        for (size_t l = 0; l < ch.taps.size(); l++) {
            x[kPrefix + i + l] += ch.taps[l] * tx[i];
        }
    }
    std::mt19937 rng(ch.seed);
    std::normal_distribution<double> g(0.0, std::sqrt(0.5 * std::pow(10.0, -ch.snr_db / 10.0)));
    std::vector<sample_t> out(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        cd v = x[i] * std::exp(cd(0.0, 2.0 * M_PI * ch.cfo_bins * i / 2048.0)) + cd(g(rng), g(rng));
        v *= kScale;
#ifdef ATSC3_FIXED_POINT
        out[i] = sample_t(float_to_q15(static_cast<float>(v.real())),
                          float_to_q15(static_cast<float>(v.imag())));
#else
        out[i] = sample_t(static_cast<float>(v.real()), static_cast<float>(v.imag()));
#endif
    }
    return out;
}

// Decodes with the buffer starting `early` samples before the true start
// (a negative value starts it late).
BootstrapDecodeResult decode(const std::vector<sample_t>& rx, int early,
                             const BootstrapDecoderConfig& cfg = BootstrapDecoderConfig()) {
    BootstrapDecoder dec(cfg);
    BootstrapDecodeResult r;
    dec.decode(rx.data() + static_cast<long>(kPrefix) - early, &r);
    return r;
}

void expect_fields(const BootstrapDecodeResult& r, const std::array<int, 3>& f) {
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(r.fields[i], f[i]) << "symbol " << i + 1;
    }
}

TEST(BootstrapDecoderTest, DecodesSignaledFields) {
    const std::array<std::array<int, 3>, 4> cases = {{
        {0x00, 2, 0},
        {0x80 | (10 << 2) | 2, 0x80 | 8, 159},
        {(30 << 2) | 1, 5, 77},
        {0x5A, 2, 0xA5 & 0x7F},
    }};
    for (const auto& f : cases) {
        BootstrapDecodeResult r = decode(receive(make_bootstrap(f), Channel{}), 0);
        EXPECT_TRUE(r.correlated);
        EXPECT_TRUE(r.termination_ok);
        EXPECT_EQ(r.timing_offset, 0);
        EXPECT_EQ(r.integer_cfo_bins, 0);
        expect_fields(r, f);
        // Table 6.3 (31 not allowed), system_bandwidth 11, Table 9.1, Table H.1.1.
        int bsr = f[1] & 0x7F;
        bool allowed = ((f[0] >> 2) & 31) != 31 && (f[0] & 3) != 3 &&
                       (bsr == 2 || bsr == 5 || bsr == 8) && f[2] < 160;
        EXPECT_EQ(r.valid, allowed);
    }
}

TEST(BootstrapDecoderTest, RelativeShiftsAreTheGrayMappedBits) {
    const std::array<int, 3> f = {0x37, 0x82, 0x9C};
    BootstrapDecodeResult r = decode(receive(make_bootstrap(f), Channel{}), 0);
    std::vector<std::vector<int>> bits = BootstrapTxModel::fields_to_bits(f);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(r.relative_shift[i], BootstrapTxModel::gray_map(bits[i]));
    }
}

TEST(BootstrapDecoderTest, MeasuresTimingErrorWithinCyclicPart) {
    const std::array<int, 3> f = {0x28, 2, 41};
    std::vector<sample_t> rx = receive(make_bootstrap(f), Channel{});
    // -400 overruns part A by 140 samples: still decodable, and the offset
    // must come back negative rather than wrapped to the end of the FFT.
    for (int early : {-400, -259, -100, -1, 1, 37, 260}) {
        BootstrapDecodeResult r = decode(rx, early);
        EXPECT_TRUE(r.valid) << early;
        EXPECT_EQ(r.timing_offset, early);
        expect_fields(r, f);
    }
}

TEST(BootstrapDecoderTest, FindsIntegerFrequencyOffset) {
    const std::array<int, 3> f = {0x28, 2, 41};
    for (int d : {-2, -1, 1, 2}) {
        Channel ch;
        ch.cfo_bins = d;
        BootstrapDecodeResult r = decode(receive(make_bootstrap(f), ch), 0);
        EXPECT_TRUE(r.valid) << d;
        EXPECT_EQ(r.integer_cfo_bins, d);
        expect_fields(r, f);
    }
}

TEST(BootstrapDecoderTest, OffsetOutsideSearchRangeIsNotDecoded) {
    Channel ch;
    ch.cfo_bins = 3;
    BootstrapDecodeResult r = decode(receive(make_bootstrap({0x28, 2, 41}), ch), 0);
    EXPECT_FALSE(r.valid);
}

TEST(BootstrapDecoderTest, DecodesAtLowSnrWithResidualOffsetAndMultipath) {
    const std::array<int, 3> f = {0x9D, 0x82, 120};
    Channel ch;
    ch.snr_db = -3.0;
    ch.cfo_bins = 1.08;  // integer bin plus 240 Hz left uncorrected
    ch.taps = {cd(0.8, 0.1), 0.0, 0.0, cd(-0.3, 0.35), 0.0, 0.0, 0.0, 0.0, cd(0.2, 0.0)};
    for (unsigned seed = 1; seed <= 5; seed++) {
        ch.seed = seed;
        BootstrapDecodeResult r = decode(receive(make_bootstrap(f), ch), 120);
        EXPECT_TRUE(r.valid) << seed;
        EXPECT_EQ(r.integer_cfo_bins, 1);
        EXPECT_EQ(r.timing_offset, 120);
        expect_fields(r, f);
    }
}

TEST(BootstrapDecoderTest, OtherMinorVersionDoesNotCorrelate) {
    BootstrapDecodeResult r =
        decode(receive(make_bootstrap({0x28, 2, 41}, /*seed=*/0x00ED), Channel{}), 0);
    EXPECT_FALSE(r.correlated);
    EXPECT_FALSE(r.valid);
}

TEST(BootstrapDecoderTest, NoiseAloneDoesNotCorrelate) {
    std::vector<cd> silence(4 * 3072, 0.0);
    for (unsigned seed = 1; seed <= 5; seed++) {
        Channel ch;
        ch.snr_db = 0.0;
        ch.seed = seed;
        EXPECT_FALSE(decode(receive(silence, ch), 0).correlated) << seed;
    }
}

TEST(BootstrapDecoderTest, MissingTerminationIsRejected) {
    // Five symbols, so symbol 3 is not inverted.
    BootstrapDecodeResult r =
        decode(receive(make_bootstrap({0x28, 2, 41}, 0x019D, 1), Channel{}), 0);
    EXPECT_TRUE(r.correlated);
    EXPECT_FALSE(r.termination_ok);
    EXPECT_FALSE(r.valid);
}

TEST(BootstrapDecoderTest, ReservedFieldValueIsRejected) {
    BootstrapDecodeResult r = decode(receive(make_bootstrap({0x28, 3, 41}), Channel{}), 0);
    EXPECT_TRUE(r.correlated);
    EXPECT_TRUE(r.termination_ok);
    EXPECT_FALSE(r.fields_ok);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.fields[1], 3);
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
