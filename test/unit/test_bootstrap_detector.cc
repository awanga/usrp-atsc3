// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_bootstrap_detector.cc — Unit tests for lib/sync/bootstrap_detector.h
//
// The input is the test-side bootstrap transmitter (bootstrap_tx_model.h,
// A/321 Sections 5.2-5.4). Expected values: the position where the test
// placed symbol 0, the frequency offset it applied, and the metric
// evaluated here by brute force from its definition (the C and B
// repetitions of Section 5.4.1). Float and fixed-point builds run the same
// cases against the same expectations.

#include "bootstrap_tx_model.h"
#include "sync/bootstrap_decoder.h"
#include "sync/bootstrap_detector.h"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

using test::BootstrapTxModel;
using cd = std::complex<double>;

constexpr double kScale = 0.15;
constexpr double kFs = 6.144e6;
constexpr size_t kStart = 7000;  // where symbol 0 begins

std::vector<cd> bootstrap(const std::array<int, 3>& fields = {0x28, 2, 41}) {
    return BootstrapTxModel::waveform(137, 0x019D, BootstrapTxModel::fields_to_bits(fields));
}

struct Stream {
    std::vector<sample_t> rx;
    std::vector<cd> clean;  // scaled, before quantization (reference input)
};

// Noise everywhere, the bootstrap (if any) from kStart, then noise-only
// tail; SNR is per sample against the bootstrap's unit power.
Stream make_stream(const std::vector<cd>& tx, double snr_db, double cfo_hz, unsigned seed,
                   size_t total = 30000) {
    Stream s;
    std::vector<cd> x(total, 0.0);
    for (size_t i = 0; i < tx.size() && kStart + i < total; i++) {
        x[kStart + i] = tx[i];
    }
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, std::sqrt(0.5 * std::pow(10.0, -snr_db / 10.0)));
    for (size_t i = 0; i < total; i++) {
        cd v = x[i] * std::exp(cd(0.0, 2.0 * M_PI * cfo_hz * i / kFs)) + cd(g(rng), g(rng));
        v *= kScale;
#ifdef ATSC3_FIXED_POINT
        sample_t q(float_to_q15(static_cast<float>(v.real())),
                   float_to_q15(static_cast<float>(v.imag())));
        s.clean.push_back(cd(q15_to_float(q.real()), q15_to_float(q.imag())));
#else
        sample_t q(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        s.clean.push_back(cd(q.real(), q.imag()));
#endif
        s.rx.push_back(q);
    }
    return s;
}

std::vector<BootstrapDetection> run(const std::vector<sample_t>& rx, BootstrapDetector* det) {
    std::vector<BootstrapDetection> out;
    for (sample_t v : rx) {
        BootstrapDetection d = det->process(v);
        if (d.detected) {
            out.push_back(d);
        }
    }
    return out;
}

// M at sample u from the definitions in the header comment.
double brute_force_metric(const std::vector<cd>& x, size_t u) {
    auto m = [&](size_t end, size_t win, size_t lag, bool shift) {
        cd p = 0.0;
        double r = 0.0;
        for (size_t v = end + 1 - win; v <= end; v++) {
            cd a = x[v], b = x[v - lag];
            cd w = shift ? std::exp(cd(0.0, -2.0 * M_PI * static_cast<double>(v % 2048) / 2048.0))
                         : 1.0;
            p += a * std::conj(b) * w;
            r += (std::norm(a) + std::norm(b)) / 2.0;
        }
        return std::norm(p) / (r * r);
    };
    return m(u - 504, 520, 2048, false) * m(u, 504, 504, true);
}

TEST(BootstrapDetectorTest, MetricMatchesDefinition) {
    Stream s = make_stream(bootstrap(), 15.0, 700.0, 3);
    BootstrapDetector det;
    const size_t peak = kStart + 3071;
    const size_t probes[] = {peak - 600, peak - 200, peak - 1, peak, peak + 1,
                             peak + 300, peak + 3072, peak + 5000};
    size_t next = 0;
    for (size_t u = 0; u < s.rx.size() && next < std::size(probes); u++) {
        det.process(s.rx[u]);
        if (u == probes[next]) {
            EXPECT_NEAR(det.get_current_metric(), brute_force_metric(s.clean, u), 2e-3) << u;
            next++;
        }
    }
    EXPECT_EQ(next, std::size(probes));
}

TEST(BootstrapDetectorTest, DetectsSymbolZeroOnceAtItsStart) {
    for (double snr : {40.0, 30.0, 10.0}) {
        Stream s = make_stream(bootstrap(), snr, 0.0, 1);
        BootstrapDetector det;
        std::vector<BootstrapDetection> d = run(s.rx, &det);
        ASSERT_EQ(d.size(), 1u) << snr;
        // Both windows match fully only at the last sample of symbol 0.
        if (snr >= 40.0) {
            EXPECT_EQ(d[0].sample_index, kStart);
        }
        EXPECT_NEAR(static_cast<double>(d[0].sample_index), static_cast<double>(kStart), 4.0);
        EXPECT_GT(d[0].metric, snr > 20 ? 0.95 : 0.6);
    }
}

TEST(BootstrapDetectorTest, EstimatesFractionalFrequencyOffset) {
    for (double cfo : {-1400.0, -300.0, 0.0, 450.0, 1200.0}) {
        Stream s = make_stream(bootstrap(), 20.0, cfo, 2);
        BootstrapDetector det;
        std::vector<BootstrapDetection> d = run(s.rx, &det);
        ASSERT_EQ(d.size(), 1u) << cfo;
        EXPECT_NEAR(d[0].cfo_hz, cfo, 25.0);
    }
}

TEST(BootstrapDetectorTest, DetectsAtMinusOneDbSnr) {
    for (unsigned seed = 1; seed <= 5; seed++) {
        Stream s = make_stream(bootstrap(), -1.0, 300.0, seed);
        BootstrapDetector det;
        std::vector<BootstrapDetection> d = run(s.rx, &det);
        ASSERT_EQ(d.size(), 1u) << seed;
        // Within the decoder's +-260 sample capture range.
        EXPECT_NEAR(static_cast<double>(d[0].sample_index), static_cast<double>(kStart), 100.0);
    }
}

TEST(BootstrapDetectorTest, NoDetectionOnNoise) {
    std::vector<cd> none;
    for (unsigned seed = 1; seed <= 3; seed++) {
        Stream s = make_stream(none, 0.0, 0.0, seed, 100000);
        BootstrapDetector det;
        EXPECT_TRUE(run(s.rx, &det).empty()) << seed;
    }
}

// A/321 5.4.2: the BCA symbols repeat B at lag 504 too, but misaligned
// with their C/A repetition, so their metric stays near 1/16 of symbol 0's.
TEST(BootstrapDetectorTest, BcaSymbolsPeakFarBelowSymbolZero) {
    std::vector<cd> tx = bootstrap();
    Stream s = make_stream(tx, 40.0, 0.0, 4);
    BootstrapDetector det;
    double cab_peak = 0.0, bca_peak = 0.0;
    for (size_t u = 0; u < s.rx.size(); u++) {
        det.process(s.rx[u]);
        double m = det.get_current_metric();
        if (u >= kStart + 2500 && u < kStart + 3600) {
            cab_peak = std::max(cab_peak, m);
        } else if (u >= kStart + 3600) {
            bca_peak = std::max(bca_peak, m);
        }
    }
    EXPECT_GT(cab_peak, 0.95);
    EXPECT_GT(bca_peak, 0.03);  // the BCA structure is visible...
    EXPECT_LT(bca_peak, 0.1);   // ...but far below symbol 0
}

TEST(BootstrapDetectorTest, NextBootstrapIsDetectedAfterHoldoff) {
    std::vector<cd> tx = bootstrap();
    std::vector<cd> two(tx);
    two.resize(tx.size() + 8000, 0.0);
    two.insert(two.end(), tx.begin(), tx.end());
    Stream s = make_stream(two, 20.0, 0.0, 10, 50000);
    BootstrapDetector det;
    std::vector<BootstrapDetection> d = run(s.rx, &det);
    ASSERT_EQ(d.size(), 2u);
    EXPECT_NEAR(static_cast<double>(d[0].sample_index), static_cast<double>(kStart), 4.0);
    EXPECT_NEAR(static_cast<double>(d[1].sample_index),
                static_cast<double>(kStart + tx.size() + 8000), 4.0);
}

TEST(BootstrapDetectorTest, SilenceGivesZeroMetric) {
    BootstrapDetector det;
    for (int i = 0; i < 8000; i++) {
        EXPECT_FALSE(det.process(sample_t(0, 0)).detected);
    }
    EXPECT_EQ(det.get_current_metric(), 0.0);
}

TEST(BootstrapDetectorTest, MetricIsBounded) {
    Stream s = make_stream(bootstrap(), 40.0, 0.0, 5);
    BootstrapDetector det;
    for (sample_t v : s.rx) {
        det.process(v);
        ASSERT_LE(det.get_current_metric(), 1.0 + 1e-3);
    }
}

TEST(BootstrapDetectorTest, SnrEstimateTracksInjectedSnr) {
    for (double snr : {5.0, 15.0}) {
        Stream s = make_stream(bootstrap(), snr, 0.0, 6);
        BootstrapDetector det;
        std::vector<BootstrapDetection> d = run(s.rx, &det);
        ASSERT_EQ(d.size(), 1u);
        EXPECT_NEAR(d[0].snr_db, snr, 1.5) << snr;
    }
}

TEST(BootstrapDetectorTest, BufferAndSampleProcessingAgree) {
    Stream s = make_stream(bootstrap(), 10.0, 200.0, 7);
    BootstrapDetector a, b;
    std::vector<BootstrapDetection> da = run(s.rx, &a);
    BootstrapDetection db = b.process(s.rx.data(), s.rx.size());
    ASSERT_EQ(da.size(), 1u);
    ASSERT_TRUE(db.detected);
    EXPECT_EQ(da[0].sample_index, db.sample_index);
    EXPECT_EQ(da[0].cfo_hz, db.cfo_hz);
}

TEST(BootstrapDetectorTest, ResetClearsHistory) {
    Stream s = make_stream(bootstrap(), 20.0, 0.0, 8);
    BootstrapDetector det;
    for (size_t i = 0; i < kStart + 2000; i++) {
        det.process(s.rx[i]);  // stop partway into symbol 0
    }
    det.reset();
    EXPECT_EQ(det.get_sample_count(), 0u);
    std::vector<BootstrapDetection> d = run(s.rx, &det);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_NEAR(static_cast<double>(d[0].sample_index), static_cast<double>(kStart), 4.0);
}

// Detector and decoder together: the coarse start and fractional CFO are
// enough for the decoder to recover the fields and the exact start.
TEST(BootstrapDetectorTest, DetectionFeedsDecoder) {
    const std::array<int, 3> f = {0x9D, 0x82, 120};
    const double cfo = 3000.0 + 650.0;  // one bin plus a fraction
    Stream s = make_stream(bootstrap(f), 3.0, cfo, 9);
    BootstrapDetector det;
    std::vector<BootstrapDetection> d = run(s.rx, &det);
    ASSERT_EQ(d.size(), 1u);
    // The fractional estimate wraps into +-1.5 kHz.
    EXPECT_NEAR(d[0].cfo_hz, cfo - 3000.0, 40.0);

    std::vector<sample_t> buf(BootstrapDecoder::kInputLength);
    for (size_t i = 0; i < buf.size(); i++) {
        size_t n = d[0].sample_index + i;
        cd v = s.clean[n] * std::exp(cd(0.0, -2.0 * M_PI * d[0].cfo_hz * n / kFs));
#ifdef ATSC3_FIXED_POINT
        buf[i] = sample_t(float_to_q15(static_cast<float>(v.real())),
                          float_to_q15(static_cast<float>(v.imag())));
#else
        buf[i] = sample_t(static_cast<float>(v.real()), static_cast<float>(v.imag()));
#endif
    }
    BootstrapDecoder dec;
    BootstrapDecodeResult r;
    ASSERT_TRUE(dec.decode(buf.data(), &r));
    EXPECT_EQ(r.integer_cfo_bins, 1);
    EXPECT_EQ(static_cast<long>(d[0].sample_index) + r.timing_offset, static_cast<long>(kStart));
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(r.fields[i], f[i]);
    }
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
