// test_timing_recovery.cc — Unit tests for lib/sync/timing_recovery.h
//
// Tests polyphase interpolator, Gardner TED, and timing recovery loop

#include <gtest/gtest.h>

#include "timing_recovery.h"

#include <cmath>
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
    timing.set_symbol_callback([&callback_count](const sample_t&, double) {
        ++callback_count;
    });

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

}  // namespace
}  // namespace sync
}  // namespace atsc3
