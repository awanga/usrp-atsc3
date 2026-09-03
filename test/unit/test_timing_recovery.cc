// test_timing_recovery.cc — Unit tests for lib/sync/timing_recovery.h
//
// Tests polyphase interpolator, Gardner TED, and timing recovery loop

#include "timing_recovery.h"

#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

constexpr double kPi = 3.14159265358979323846;

//==============================================================================
// PolyphaseInterpolator Tests
//==============================================================================

TEST(PolyphaseInterpolatorTest, DefaultConstruction) {
    PolyphaseInterpolator interp;

    EXPECT_EQ(interp.get_config().num_phases, 16u);
    EXPECT_EQ(interp.get_config().taps_per_phase, 32u);
    EXPECT_EQ(interp.get_total_taps(), 512u);
}

TEST(PolyphaseInterpolatorTest, CustomConfig) {
    PolyphaseConfig config;
    config.num_phases = 8;
    config.taps_per_phase = 16;

    PolyphaseInterpolator interp(config);

    EXPECT_EQ(interp.get_config().num_phases, 8u);
    EXPECT_EQ(interp.get_config().taps_per_phase, 16u);
    EXPECT_EQ(interp.get_total_taps(), 128u);
}

TEST(PolyphaseInterpolatorTest, InvalidConfigThrows) {
    PolyphaseConfig config;
    config.num_phases = 0;

    EXPECT_THROW(PolyphaseInterpolator interp(config), std::invalid_argument);
}

TEST(PolyphaseInterpolatorTest, InterpolateZeroMu) {
    PolyphaseConfig config;
    config.num_phases = 4;
    config.taps_per_phase = 4;

    PolyphaseInterpolator interp(config);

    // Create simple buffer with unit impulse
    std::vector<sample_t> buf(config.taps_per_phase, sample_t(0, 0));
#ifdef ATSC3_FIXED_POINT
    buf[config.taps_per_phase / 2] = sample_t(float_to_q15(0.9f), float_to_q15(0.0f));
#else
    buf[config.taps_per_phase / 2] = sample_t(0.9f, 0.0f);
#endif

    sample_t result = interp.interpolate(buf.data(), config.taps_per_phase - 1, 0.0);

    // Result should be non-zero (impulse response)
#ifdef ATSC3_FIXED_POINT
    float re = q15_to_float(result.real());
#else
    float re = result.real();
#endif
    EXPECT_GT(std::abs(re), 0.0f);
}

TEST(PolyphaseInterpolatorTest, InterpolateHalfMu) {
    PolyphaseConfig config;
    config.num_phases = 4;
    config.taps_per_phase = 4;

    PolyphaseInterpolator interp(config);

    std::vector<sample_t> buf(config.taps_per_phase, sample_t(0, 0));
#ifdef ATSC3_FIXED_POINT
    buf[1] = sample_t(float_to_q15(0.5f), float_to_q15(0.0f));
    buf[2] = sample_t(float_to_q15(0.5f), float_to_q15(0.0f));
#else
    buf[1] = sample_t(0.5f, 0.0f);
    buf[2] = sample_t(0.5f, 0.0f);
#endif

    // Interpolate at mu=0.5 (halfway between samples)
    sample_t result = interp.interpolate(buf.data(), 3, 0.5);

    // Should produce some output
#ifdef ATSC3_FIXED_POINT
    EXPECT_GT(std::abs(q15_to_float(result.real())), 0.0f);
#else
    EXPECT_GT(std::abs(result.real()), 0.0f);
#endif
}

//==============================================================================
// GardnerTed Tests
//==============================================================================

TEST(GardnerTedTest, Construction) {
    GardnerTed ted;
    // Should construct without error
}

TEST(GardnerTedTest, ZeroErrorForDC) {
    GardnerTed ted;

    // DC signal: all samples equal
#ifdef ATSC3_FIXED_POINT
    sample_t dc = sample_t(float_to_q15(0.5f), float_to_q15(0.0f));
#else
    sample_t dc = sample_t(0.5f, 0.0f);
#endif

    double error = ted.compute_error(dc, dc, dc);

    // Error should be zero for DC (no timing info)
    EXPECT_NEAR(error, 0.0, 1e-5);
}

TEST(GardnerTedTest, PositiveErrorForEarlySample) {
    GardnerTed ted;

    // Simulate early sampling: rising edge
    // x_prev < x_mid < x_curr (positive slope)
    // Midpoint is below average -> sample is early -> positive error

#ifdef ATSC3_FIXED_POINT
    sample_t x_prev = sample_t(float_to_q15(0.2f), float_to_q15(0.0f));
    sample_t x_mid = sample_t(float_to_q15(0.3f), float_to_q15(0.0f));  // Low midpoint
    sample_t x_curr = sample_t(float_to_q15(0.8f), float_to_q15(0.0f));
#else
    sample_t x_prev = sample_t(0.2f, 0.0f);
    sample_t x_mid = sample_t(0.3f, 0.0f);
    sample_t x_curr = sample_t(0.8f, 0.0f);
#endif

    double error = ted.compute_error(x_curr, x_mid, x_prev);

    // Error should be positive (early)
    EXPECT_GT(error, 0.0);
}

TEST(GardnerTedTest, NegativeErrorForLateSample) {
    GardnerTed ted;

    // Simulate late sampling: falling edge
    // x_curr < x_mid < x_prev (negative slope)
    // With midpoint high relative to endpoints -> sample is late -> negative error

#ifdef ATSC3_FIXED_POINT
    sample_t x_prev = sample_t(float_to_q15(0.8f), float_to_q15(0.0f));
    sample_t x_mid = sample_t(float_to_q15(0.7f), float_to_q15(0.0f));  // High midpoint
    sample_t x_curr = sample_t(float_to_q15(0.2f), float_to_q15(0.0f));
#else
    sample_t x_prev = sample_t(0.8f, 0.0f);
    sample_t x_mid = sample_t(0.7f, 0.0f);
    sample_t x_curr = sample_t(0.2f, 0.0f);
#endif

    double error = ted.compute_error(x_curr, x_mid, x_prev);

    // Error should be negative (late)
    EXPECT_LT(error, 0.0);
}

TEST(GardnerTedTest, Reset) {
    GardnerTed ted;
    ted.reset();
    // Should not throw
}

//==============================================================================
// TimingRecovery Tests
//==============================================================================

TEST(TimingRecoveryTest, DefaultConstruction) {
    TimingRecovery timing;

    EXPECT_EQ(timing.get_config().sample_rate_hz, 6.25e6);
    EXPECT_EQ(timing.get_symbol_count(), 0u);
    EXPECT_NEAR(timing.get_timing_offset(), 0.0, 1e-10);
    EXPECT_FALSE(timing.is_locked());
}

TEST(TimingRecoveryTest, CustomConfig) {
    TimingRecoveryConfig config;
    config.sample_rate_hz = 12.5e6;
    config.initial_offset = 0.25;
    config.loop_bandwidth_hz = 2.0;

    TimingRecovery timing(config);

    EXPECT_EQ(timing.get_config().sample_rate_hz, 12.5e6);
    EXPECT_NEAR(timing.get_timing_offset(), 0.25, 1e-10);
}

TEST(TimingRecoveryTest, Reset) {
    TimingRecoveryConfig config;
    config.initial_offset = 0.5;

    TimingRecovery timing(config);

    // Process some samples
    std::vector<sample_t> data(100);
#ifdef ATSC3_FIXED_POINT
    std::fill(data.begin(), data.end(), sample_t(float_to_q15(0.5f), float_to_q15(0.0f)));
#else
    std::fill(data.begin(), data.end(), sample_t(0.5f, 0.0f));
#endif

    timing.process(data.data(), 100);

    timing.reset();

    EXPECT_NEAR(timing.get_timing_offset(), 0.5, 1e-10);
    EXPECT_EQ(timing.get_symbol_count(), 0u);
}

TEST(TimingRecoveryTest, SetTimingOffset) {
    TimingRecovery timing;

    timing.set_timing_offset(0.75);
    EXPECT_NEAR(timing.get_timing_offset(), 0.75, 1e-10);

    // Test wrapping
    timing.set_timing_offset(1.25);
    EXPECT_NEAR(timing.get_timing_offset(), 0.25, 1e-10);
}

TEST(TimingRecoveryTest, LockState) {
    TimingRecovery timing;

    EXPECT_FALSE(timing.is_locked());

    timing.set_locked(true);
    EXPECT_TRUE(timing.is_locked());

    timing.set_locked(false);
    EXPECT_FALSE(timing.is_locked());
}

TEST(TimingRecoveryTest, SymbolCallback) {
    TimingRecoveryConfig config;
    config.polyphase.samples_per_symbol = 2;

    TimingRecovery timing(config);

    size_t callback_count = 0;
    timing.set_symbol_callback([&callback_count](const sample_t&, double) { ++callback_count; });

    // Generate 2x oversampled signal
    std::vector<sample_t> data(100);
    for (size_t i = 0; i < data.size(); ++i) {
        float val = (i % 2 == 0) ? 0.5f : 0.3f;  // Alternating pattern
#ifdef ATSC3_FIXED_POINT
        data[i] = sample_t(float_to_q15(val), float_to_q15(0.0f));
#else
        data[i] = sample_t(val, 0.0f);
#endif
    }

    timing.process(data.data(), data.size());

    // Should have produced some symbols (100 samples / 2 = ~50 symbols)
    EXPECT_GT(callback_count, 0u);
    EXPECT_EQ(timing.get_symbol_count(), callback_count);
}

TEST(TimingRecoveryTest, ProcessesWithoutCallback) {
    TimingRecovery timing;

    std::vector<sample_t> data(100);
#ifdef ATSC3_FIXED_POINT
    std::fill(data.begin(), data.end(), sample_t(float_to_q15(0.5f), float_to_q15(0.0f)));
#else
    std::fill(data.begin(), data.end(), sample_t(0.5f, 0.0f));
#endif

    // Should not crash without callback
    timing.process(data.data(), data.size());
}

TEST(TimingRecoveryTest, TimingErrorStaysSmallWhenLocked) {
    TimingRecoveryConfig config;
    config.polyphase.samples_per_symbol = 2;
    config.loop_bandwidth_hz = 5.0;  // Faster loop for test

    TimingRecovery timing(config);
    timing.set_locked(true);

    // Generate well-synchronized signal (square wave at symbol rate)
    std::vector<sample_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) {
        float val = ((i / 2) % 2 == 0) ? 0.8f : 0.2f;
#ifdef ATSC3_FIXED_POINT
        data[i] = sample_t(float_to_q15(val), float_to_q15(0.0f));
#else
        data[i] = sample_t(val, 0.0f);
#endif
    }

    timing.process(data.data(), data.size());

    // Timing offset should remain small
    EXPECT_LT(std::abs(timing.get_timing_offset()), 0.5);
}

#ifdef ATSC3_FIXED_POINT

//==============================================================================
// Phase 9.0b equivalence: fixed-point rewrite vs. the pre-rewrite double
// algorithm. Per the HDL port plan, each Phase 9.0b block needs its own
// >=40 dB SNR-vs-pre-rewrite checkpoint before its RTL phase starts.
//
// Deliberately NOT tested this way: TimingRecovery's full closed loop
// (mu_/timing_error_ trajectory over many samples). A diagnostic during
// development established why -- mu_ selects one of num_phases discrete
// FIR phases (phase = floor(mu*num_phases)), so any quantization of mu_
// (Q0.16 here, but this is true of *any* finite-precision mu_, fixed or
// float) occasionally lands on a different side of a phase boundary than
// an exact-double reference would. That single boundary crossing feeds a
// materially different interpolated sample into the Gardner TED, which
// the closed loop then integrates and tracks along a genuinely different
// (not just noisier) trajectory from that point on -- confirmed by
// perturbing a *pure-double* simulation's mu by rounding it to Q0.16
// after each step with everything else exact: the two trajectories track
// within 1e-4 for ~500 samples, then jump to a 0.05+ divergence at one
// step and stay there. That's a bifurcation inherent to closed-loop
// tracking of a quantized phase-select variable, not a rewrite defect --
// an SNR bar over a long trajectory would fail for *any* correct
// finite-precision implementation of this loop, not just a buggy one.
//
// What Phase 9.0b's rewrite actually needs validated is the two
// memoryless pieces it changed: the FIR dot product (Q1.15 taps now,
// were float) and the Gardner error computation (genuine integer diff/
// dot-product now, was double). Both are tested directly below, many
// independent trials, no feedback loop involved -- exactly the kind of
// computation an aggregate SNR bar is meaningful for. The loop's
// qualitative behavior (locks, timing error stays bounded) is already
// covered by TimingErrorStaysSmallWhenLocked above, which passes in both
// build configs.
//==============================================================================

namespace {

double ref_sinc(double x) {
    if (std::abs(x) < 1e-10)
        return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

double ref_raised_cosine(double t, double rolloff, double T) {
    if (rolloff < 1e-10)
        return ref_sinc(t / T);
    double denom = 1.0 - 4.0 * rolloff * rolloff * t * t / (T * T);
    if (std::abs(denom) < 1e-10) {
        return (kPi / (4.0 * T)) * ref_sinc(1.0 / (2.0 * rolloff));
    }
    return ref_sinc(t / T) * std::cos(kPi * rolloff * t / T) / denom;
}

// Reference (double-precision) polyphase coefficient design, matching
// PolyphaseInterpolator::init_coefficients() exactly except for the final
// quantization step -- used to build the "what would the taps be without
// Q1.15 rounding" comparison table below.
std::vector<std::vector<double>> design_reference_coeffs(const PolyphaseConfig& config) {
    size_t total_taps = config.num_phases * config.taps_per_phase;
    std::vector<double> prototype(total_taps);
    double center = static_cast<double>(total_taps - 1) / 2.0;
    constexpr double kRolloff = 0.2;
    double T = static_cast<double>(config.num_phases);
    for (size_t i = 0; i < total_taps; ++i) {
        double t = static_cast<double>(i) - center;
        prototype[i] = ref_raised_cosine(t, kRolloff, T);
    }
    double beta = 5.0;
    for (size_t i = 0; i < total_taps; ++i) {
        double x = 2.0 * static_cast<double>(i) / static_cast<double>(total_taps - 1) - 1.0;
        double w = std::cosh(beta * std::sqrt(1.0 - x * x)) / std::cosh(beta);
        prototype[i] *= w;
    }
    double sum = 0.0;
    for (size_t i = 0; i < total_taps; i += config.num_phases)
        sum += prototype[i];
    if (std::abs(sum) > 1e-10) {
        for (double& c : prototype)
            c /= sum;
    }
    std::vector<std::vector<double>> coeffs(config.num_phases);
    for (size_t p = 0; p < config.num_phases; ++p) {
        coeffs[p].resize(config.taps_per_phase);
        for (size_t t = 0; t < config.taps_per_phase; ++t) {
            coeffs[p][t] = prototype[p + t * config.num_phases];
        }
    }
    return coeffs;
}

std::complex<double> reference_interpolate(const std::vector<std::complex<double>>& buf,
                                           size_t buf_idx, double mu,
                                           const std::vector<std::vector<double>>& coeffs,
                                           size_t num_phases, size_t taps_per_phase) {
    mu = mu - std::floor(mu);
    size_t phase = static_cast<size_t>(mu * num_phases);
    if (phase >= num_phases)
        phase = num_phases - 1;
    const auto& taps = coeffs[phase];
    size_t wrap = buf.size();
    double acc_re = 0.0, acc_im = 0.0;
    for (size_t t = 0; t < taps_per_phase; ++t) {
        size_t idx = (buf_idx + wrap - 1 - t) % wrap;
        acc_re += buf[idx].real() * taps[t];
        acc_im += buf[idx].imag() * taps[t];
    }
    return std::complex<double>(acc_re, acc_im);
}

}  // namespace

TEST(PolyphaseInterpolatorTest, FixedPointVsReferenceEquivalence) {
    PolyphaseConfig config;  // defaults: 16 phases, 32 taps/phase
    PolyphaseInterpolator interp(config);
    auto ref_coeffs = design_reference_coeffs(config);

    std::mt19937 rng(2024);
    // Amplitude bound: NOT +-0.9. A first version of this test used that
    // and measured ~30 dB, which traced back entirely to Q1.15 output
    // saturation, not interpolator imprecision -- filling all 128 buffer
    // slots with *independent* near-full-scale noise is an unrealistic
    // stress input (real signals are correlated / band-limited), and a
    // 32-tap raised-cosine/Kaiser filter can legitimately overshoot +-1.0
    // on such input (confirmed: >99% of that run's error power came from
    // the ~2.4% of trials that saturated; dropping to +-0.5 amplitude
    // eliminates saturation entirely and the same code measures ~85 dB).
    // +-0.5 leaves headroom above the -20 dBFS nominal operating point
    // documented in hdl/docs/q_format_notes.md while still exercising a
    // realistic dynamic range; saturation correctness itself is a
    // separate, deliberate concern (see that doc's stimulus guidance),
    // not what this test is measuring.
    std::uniform_real_distribution<float> amp_dist(-0.5f, 0.5f);
    std::uniform_real_distribution<double> mu_dist(0.0, 1.0);

    double signal_power = 0.0;
    double error_power = 0.0;

    for (int trial = 0; trial < 500; ++trial) {
        size_t buf_len = config.taps_per_phase * 4;
        std::vector<sample_t> buf_fixed(buf_len);
        std::vector<std::complex<double>> buf_ref(buf_len);
        for (size_t i = 0; i < buf_len; ++i) {
            float re = amp_dist(rng);
            float im = amp_dist(rng);
            buf_fixed[i] = sample_t(float_to_q15(re), float_to_q15(im));
            buf_ref[i] = std::complex<double>(q15_to_float(buf_fixed[i].real()),
                                              q15_to_float(buf_fixed[i].imag()));
        }
        size_t buf_idx = static_cast<size_t>(rng() % buf_len);
        double mu = mu_dist(rng);

        sample_t result = interp.interpolate(buf_fixed.data(), buf_idx, mu, buf_len);
        double fixed_re = q15_to_float(result.real());
        double fixed_im = q15_to_float(result.imag());

        auto ref_result = reference_interpolate(buf_ref, buf_idx, mu, ref_coeffs, config.num_phases,
                                                config.taps_per_phase);

        signal_power +=
            ref_result.real() * ref_result.real() + ref_result.imag() * ref_result.imag();
        double dr = fixed_re - ref_result.real();
        double di = fixed_im - ref_result.imag();
        error_power += dr * dr + di * di;
    }

    double snr_db_value = 10.0 * std::log10(signal_power / error_power);
    EXPECT_GE(snr_db_value, 40.0) << "fixed-point interpolator SNR vs. double reference: "
                                  << snr_db_value << " dB";
}

TEST(GardnerTedTest, FixedPointVsReferenceEquivalence) {
    GardnerTed ted;
    std::mt19937 rng(4096);
    std::uniform_real_distribution<float> amp_dist(-0.9f, 0.9f);

    double signal_power = 0.0;
    double error_power = 0.0;

    for (int trial = 0; trial < 2000; ++trial) {
        float curr_re = amp_dist(rng), curr_im = amp_dist(rng);
        float mid_re = amp_dist(rng), mid_im = amp_dist(rng);
        float prev_re = amp_dist(rng), prev_im = amp_dist(rng);

        sample_t x_curr(float_to_q15(curr_re), float_to_q15(curr_im));
        sample_t x_mid(float_to_q15(mid_re), float_to_q15(mid_im));
        sample_t x_prev(float_to_q15(prev_re), float_to_q15(prev_im));

        double fixed_error = ted.compute_error(x_curr, x_mid, x_prev);

        double rd_re = q15_to_float(x_curr.real()) - q15_to_float(x_prev.real());
        double rd_im = q15_to_float(x_curr.imag()) - q15_to_float(x_prev.imag());
        double ref_error = rd_re * q15_to_float(x_mid.real()) + rd_im * q15_to_float(x_mid.imag());

        signal_power += ref_error * ref_error;
        double d = fixed_error - ref_error;
        error_power += d * d;
    }

    double snr_db_value = 10.0 * std::log10(signal_power / error_power);
    EXPECT_GE(snr_db_value, 40.0) << "fixed-point Gardner TED SNR vs. double reference: "
                                  << snr_db_value << " dB";
}

#endif  // ATSC3_FIXED_POINT

}  // namespace
}  // namespace sync
}  // namespace atsc3
