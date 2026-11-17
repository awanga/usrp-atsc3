// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_resampler.cc — Unit tests for lib/dsp/resampler.h
//
// Expected values come from independent models, not from the resampler:
// exact rational positions (output k at input time k M / L), the exact
// complex exponential at each output's input time, a DFT-bin measurement
// for images, and for the end-to-end case the A/321 test transmitter
// (bootstrap_tx_model.h) converted to 6.25 MS/s by a separate long
// double-precision sinc interpolator in this file, then decoded by the
// bootstrap detector and decoder.

#include "bootstrap_tx_model.h"
#include "dsp/resampler.h"
#include "sync/bootstrap_decoder.h"
#include "sync/bootstrap_detector.h"

#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>
#include <stdexcept>
#include <vector>

namespace atsc3 {
namespace dsp {
namespace {

using cd = std::complex<double>;

sample_t to_sample(cd v) {
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

std::vector<sample_t> run(Resampler& r, const std::vector<sample_t>& in, size_t chunk = 1000) {
    std::vector<sample_t> out;
    std::vector<sample_t> buf(chunk * 2 + 4);
    for (size_t pos = 0; pos < in.size(); pos += chunk) {
        size_t n = std::min(chunk, in.size() - pos);
        size_t got = r.process(in.data() + pos, n, buf.data(), buf.size());
        out.insert(out.end(), buf.begin(), buf.begin() + got);
    }
    return out;
}

// Error power of y against a * exp(j 2 pi f x_k / fin), relative to a^2,
// in dB, over outputs whose filter span lies inside the input.
double tone_error_db(const std::vector<sample_t>& y, double a, double f, double fin, uint64_t l,
                     uint64_t m, size_t skip) {
    double err = 0.0;
    size_t count = 0;
    for (size_t k = skip; k < y.size(); ++k) {
        double x = static_cast<double>(k) * static_cast<double>(m) / static_cast<double>(l);
        cd want = a * std::exp(cd(0.0, 2.0 * M_PI * f * x / fin));
        err += std::norm(to_cd(y[k]) - want);
        ++count;
    }
    return 10.0 * std::log10(err / count / (a * a));
}

std::vector<sample_t> tone(size_t n, double a, double f, double fs) {
    std::vector<sample_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = to_sample(a * std::exp(cd(0.0, 2.0 * M_PI * f * i / fs)));
    }
    return v;
}

TEST(ResamplerTest, RatioInLowestTerms) {
    Resampler down;  // 6.25 -> 6.144 MS/s
    EXPECT_EQ(down.interpolation(), 3072u);
    EXPECT_EQ(down.decimation(), 3125u);
    ResamplerConfig c;
    c.output_rate_hz = 6912000;  // (2 + 16) x 0.384 MHz
    Resampler up(c);
    EXPECT_EQ(up.interpolation(), 3456u);
    EXPECT_EQ(up.decimation(), 3125u);
}

TEST(ResamplerTest, RejectsInvalidConfig) {
    ResamplerConfig c;
    c.taps_per_phase = 63;
    EXPECT_THROW(Resampler r(c), std::invalid_argument);
    c = ResamplerConfig();
    c.output_rate_hz = 0;
    EXPECT_THROW(Resampler r(c), std::invalid_argument);
    c = ResamplerConfig();
    c.stopband_hz = c.passband_hz;
    EXPECT_THROW(Resampler r(c), std::invalid_argument);
}

// Output k exists once input floor(k M / L) + taps/2 has arrived.
TEST(ResamplerTest, OutputCountFollowsExactPositions) {
    for (uint64_t out_rate : {uint64_t{6144000}, uint64_t{6912000}}) {
        ResamplerConfig c;
        c.output_rate_hz = out_rate;
        Resampler r(c);
        std::vector<sample_t> in(50000, sample_t{});
        std::vector<sample_t> y = run(r, in, 777);
        const uint64_t l = out_rate / 2000, m = 3125;  // gcd(6.25e6, rate) = 2000
        uint64_t expected = 0;
        // Phase rounding can move the position to the next sample only
        // when the fraction is within half a branch of 1.
        for (uint64_t k = 0;; ++k) {
            uint64_t t = k * m;
            uint64_t idx = t / l;
            if (2 * (t % l) * c.num_phases + l >= 2 * l * c.num_phases) {
                ++idx;
            }
            if (idx + c.taps_per_phase / 2 >= in.size()) {
                break;
            }
            ++expected;
        }
        EXPECT_EQ(y.size(), expected) << out_rate;
        EXPECT_EQ(r.next_position().index, expected * m / l);
    }
}

// Passband tones come out at the right phase for each output's exact
// input time: timing (1/1024 sample) and passband ripple bound the error.
TEST(ResamplerTest, PassbandTonesAtExactPositions) {
    const double fin = 6.25e6;
    for (uint64_t out_rate : {uint64_t{6144000}, uint64_t{6912000}}) {
        for (double f : {-2.9e6, -1.0e6, 0.3e6, 2.0e6, 2.9e6}) {
            ResamplerConfig c;
            c.output_rate_hz = out_rate;
            Resampler r(c);
            std::vector<sample_t> y = run(r, tone(20000, 0.5, f, fin));
            double e = tone_error_db(y, 0.5, f, fin, r.interpolation(), r.decimation(), 64);
            // Measured -55 dB at +-2.9 MHz, -70 dB at 0.3 MHz, both builds.
            EXPECT_LT(e, -52.0) << out_rate << " " << f;
        }
    }
}

// 6.25 -> 6.912 MS/s: an input tone at f also appears at f - 6.25 MHz in
// the output band unless the filter stops it. Measured by correlating
// with the image frequency.
TEST(ResamplerTest, ImagesAttenuated) {
    const double fin = 6.25e6, fout = 6.912e6;
    ResamplerConfig c;
    c.output_rate_hz = 6912000;
    Resampler r(c);
    const double f = 2.9e6;  // image at -3.35 MHz, inside +-3.456 MHz
    std::vector<sample_t> y = run(r, tone(40000, 0.5, f, fin));
    auto level = [&](double freq) {
        cd acc = 0.0;
        size_t n = 0;
        for (size_t k = 64; k < y.size(); ++k, ++n) {
            acc += to_cd(y[k]) * std::exp(cd(0.0, -2.0 * M_PI * freq * k / fout));
        }
        return std::abs(acc) / n;
    };
    double wanted = level(f);
    double image = level(f - fin);
    EXPECT_NEAR(wanted, 0.5, 0.01);
    EXPECT_LT(20.0 * std::log10(image / wanted), -65.0);  // measured -70 dB
}

// Equal rates put every output on an input sample (branch 0), and at the
// default cutoff (6 dB point at the input Nyquist) branch 0 is a single
// tap: the input comes back unchanged (fixed point: DC gain 32767/32768,
// within one LSB).
TEST(ResamplerTest, SameRateIsPassThrough) {
    ResamplerConfig c;
    c.output_rate_hz = c.input_rate_hz;
    Resampler r(c);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-0.99, 0.99);
    std::vector<sample_t> in(5000);
    for (auto& s : in) {
        s = to_sample(cd(u(rng), u(rng)));
    }
    std::vector<sample_t> y = run(r, in);
    ASSERT_EQ(y.size(), in.size() - c.taps_per_phase / 2);
    for (size_t k = 0; k < y.size(); ++k) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_LE(std::abs(y[k].real() - in[k].real()), 1) << k;
        EXPECT_LE(std::abs(y[k].imag() - in[k].imag()), 1) << k;
#else
        EXPECT_NEAR(y[k].real(), in[k].real(), 1e-6) << k;
        EXPECT_NEAR(y[k].imag(), in[k].imag(), 1e-6) << k;
#endif
    }
}

// Equivalence: each output against a reference built from the stored
// branch coefficients and exact position arithmetic -- bit-exact in fixed
// point (Q1.15 products summed, rounded half up, saturated), 1e-5 in
// float. Checks the arithmetic port, not the filter design.
TEST(ResamplerTest, MatchesReferenceArithmetic) {
    ResamplerConfig c;
    c.output_rate_hz = 6912000;
    Resampler r(c);
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(-0.9, 0.9);
    std::vector<sample_t> in(4000);
    for (auto& s : in) {
        s = to_sample(cd(u(rng), u(rng)));
    }
    std::vector<sample_t> y = run(r, in);
    const uint64_t l = r.interpolation(), m = r.decimation(), n_ph = c.num_phases;
    const long half = static_cast<long>(c.taps_per_phase / 2);
    ASSERT_GT(y.size(), 1000u);
    for (size_t k = 0; k < y.size(); ++k) {
        uint64_t t = k * m;
        uint64_t idx = t / l;
        uint64_t ph = (2 * (t % l) * n_ph + l) / (2 * l);
        if (ph == n_ph) {
            ph = 0;
            ++idx;
        }
        const auto& b = r.branch(ph);
        double re = 0.0, im = 0.0;
        int64_t qre = 0, qim = 0;
        for (long j = 0; j < 2 * half; ++j) {
            long n = static_cast<long>(idx) - half + 1 + j;
            if (n < 0) {
                continue;
            }
            re += static_cast<double>(b[j]) * in[n].real();
            im += static_cast<double>(b[j]) * in[n].imag();
            qre += static_cast<int64_t>(b[j]) * in[n].real();
            qim += static_cast<int64_t>(b[j]) * in[n].imag();
        }
#ifdef ATSC3_FIXED_POINT
        auto q = [](int64_t a) {
            int64_t v = (a + 16384) >> 15;
            return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
        };
        (void)re;
        (void)im;
        EXPECT_EQ(y[k], sample_t(q(qre), q(qim))) << k;
#else
        (void)qre;
        (void)qim;
        EXPECT_NEAR(y[k].real(), re, 1e-5) << k;
        EXPECT_NEAR(y[k].imag(), im, 1e-5) << k;
#endif
    }
}

// Feeding in odd-sized chunks, or with a tight output buffer, gives the
// same samples as one large call.
TEST(ResamplerTest, ChunkingDoesNotMatter) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-0.4, 0.4);
    std::vector<sample_t> in(12000);
    for (auto& s : in) {
        s = to_sample(cd(u(rng), u(rng)));
    }
    Resampler a, b;
    std::vector<sample_t> ya = run(a, in, in.size());
    std::vector<sample_t> yb;
    sample_t small[3];
    for (size_t pos = 0; pos < in.size();) {
        size_t n = std::min<size_t>(37, in.size() - pos);
        size_t got = b.process(in.data() + pos, n, small, 3);
        yb.insert(yb.end(), small, small + got);
        pos += n;
        while ((got = b.process(nullptr, 0, small, 3)) > 0) {
            yb.insert(yb.end(), small, small + got);
        }
    }
    ASSERT_EQ(ya.size(), yb.size());
    for (size_t k = 0; k < ya.size(); ++k) {
        EXPECT_EQ(ya[k], yb[k]) << k;
    }
}

TEST(ResamplerTest, BacklogBeyondHistoryFailsVisibly) {
    ResamplerConfig c;
    c.history = 1000;
    Resampler r(c);
    std::vector<sample_t> in(3000, sample_t{});
    sample_t out[1];
    r.process(in.data(), in.size(), out, 1);
    EXPECT_THROW(r.process(nullptr, 0, out, 1), std::length_error);
}

// set_position: the next output lands at the requested input time,
// rounded to 1/L; positions beyond the history are refused.
TEST(ResamplerTest, SetPositionStartsAtThatInputTime) {
    const double fin = 6.25e6, f = 1.1e6;
    std::vector<sample_t> in = tone(30000, 0.5, f, fin);
    ResamplerConfig c;
    c.output_rate_hz = 6912000;
    Resampler r(c);
    std::vector<sample_t> buf(40000);
    r.process(in.data(), 20000, buf.data(), buf.size());
    // A time on the 6.144 MS/s grid: 6.144-rate sample 17000 = input
    // 17000 x 3125 / 3072.
    ResamplerPosition p{17000ull * 3125 / 3072, 17000ull * 3125 % 3072, 3072};
    ASSERT_TRUE(r.set_position(p));
    ResamplerPosition q = r.next_position();
    EXPECT_EQ(q.den, 3456u);
    EXPECT_EQ(q.index, p.index);
    double want_x = p.index + static_cast<double>(p.frac) / p.den;
    EXPECT_LE(std::abs(q.index + static_cast<double>(q.frac) / q.den - want_x), 0.5 / 3456 + 1e-12);
    size_t got = r.process(in.data() + 20000, 10000, buf.data(), buf.size());
    ASSERT_GT(got, 100u);
    double err = 0.0;
    for (size_t k = 0; k < got; ++k) {
        double x = q.index + (q.frac + static_cast<double>(k) * 3125) / 3456.0;
        err += std::norm(to_cd(buf[k]) - 0.5 * std::exp(cd(0.0, 2.0 * M_PI * f * x / fin)));
    }
    EXPECT_LT(10.0 * std::log10(err / got / 0.25), -45.0);

    ResamplerPosition old{100, 0, 1};
    EXPECT_FALSE(r.set_position(old));
}

// 4/7 of a sample is 1974.857 / 3456: the nearest 1/L, not truncation.
TEST(ResamplerTest, SetPositionRoundsToNearest) {
    ResamplerConfig c;
    c.output_rate_hz = 6912000;
    Resampler r(c);
    ASSERT_TRUE(r.set_position(ResamplerPosition{10, 4, 7}));
    EXPECT_EQ(r.next_position().index, 10u);
    EXPECT_EQ(r.next_position().frac, 1975u);
    // A fraction of a whole sample or more carries into the index:
    // 13/7 = 1 + 6/7, and 6/7 is 2962.29 / 3456.
    ASSERT_TRUE(r.set_position(ResamplerPosition{10, 13, 7}));
    EXPECT_EQ(r.next_position().index, 11u);
    EXPECT_EQ(r.next_position().frac, 2962u);
}

// Band-limited conversion of a 6.144 MS/s signal to the 6.25 MS/s grid,
// for the stimulus only: 256-tap Blackman-Harris-windowed sinc in double
// (a different design from the resampler's Kaiser branches).
std::vector<cd> to_6250k(const std::vector<cd>& x) {
    const long half = 128;
    size_t n_out = static_cast<size_t>(std::floor((x.size() - 1) * 6250.0 / 6144.0));
    std::vector<cd> y(n_out);
    for (size_t n = 0; n < n_out; ++n) {
        double t = n * 6144.0 / 6250.0;  // in 6.144 MS/s samples
        long c = static_cast<long>(std::floor(t));
        cd acc = 0.0;
        for (long i = c - half + 1; i <= c + half; ++i) {
            if (i < 0 || i >= static_cast<long>(x.size())) {
                continue;
            }
            double a = t - i;
            double r = a / half;  // window over a in [-half, half]
            double w = 0.35875 + 0.48829 * std::cos(M_PI * r) + 0.14128 * std::cos(2 * M_PI * r) +
                       0.01168 * std::cos(3 * M_PI * r);
            double s = a == 0.0 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            acc += x[i] * s * w;
        }
        y[n] = acc;
    }
    return y;
}

// A/321 bootstrap transmitted at 6.144 MS/s, received at 6.25 MS/s,
// resampled back: detection and decoding find it at the transmitted
// start with the transmitted fields.
TEST(ResamplerTest, BootstrapSurvivesTheRadioRate) {
    using test::BootstrapTxModel;
    const std::array<int, 3> fields = {0x28, 2, 41};
    std::vector<cd> tx =
        BootstrapTxModel::waveform(137, 0x019D, BootstrapTxModel::fields_to_bits(fields));
    const size_t start = 5000;
    std::vector<cd> x(start + tx.size() + 6000, 0.0);
    std::mt19937 rng(11);
    std::normal_distribution<double> g(0.0, std::sqrt(0.5 * 0.01));  // 20 dB SNR
    for (size_t i = 0; i < x.size(); ++i) {
        x[i] = (i >= start && i < start + tx.size() ? tx[i - start] : 0.0) + cd(g(rng), g(rng));
    }
    std::vector<cd> rx = to_6250k(x);
    std::vector<sample_t> in;
    for (const cd& v : rx) {
        in.push_back(to_sample(0.15 * v));
    }

    Resampler r;  // 6.25 -> 6.144 MS/s
    std::vector<sample_t> y = run(r, in);
    // Output k is at 6.25 MS/s input time k x 3125 / 3072, which is
    // 6.144 MS/s time k: the bootstrap starts at output `start`.
    sync::BootstrapDetector det;
    sync::BootstrapDetection d;
    for (const sample_t& s : y) {
        sync::BootstrapDetection e = det.process(s);
        if (e.detected) {
            d = e;
            break;
        }
    }
    ASSERT_TRUE(d.detected);
    ASSERT_LE(d.sample_index + sync::BootstrapDecoder::kInputLength, y.size());
    sync::BootstrapDecoder dec;
    sync::BootstrapDecodeResult res;
    ASSERT_TRUE(dec.decode(y.data() + d.sample_index, &res))
        << res.correlated << res.termination_ok << res.fields_ok;
    EXPECT_EQ(static_cast<long>(d.sample_index) + res.timing_offset, static_cast<long>(start));
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(res.fields[i], fields[i]) << i;
    }
}

}  // namespace
}  // namespace dsp
}  // namespace atsc3
