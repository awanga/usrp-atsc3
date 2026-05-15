// test_freq_correction.cc — Unit tests for lib/sync/freq_correction.h
//
// Tests CFO correction and pilot phase tracking

#include "freq_correction.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

constexpr double kPi = 3.14159265358979323846;

//==============================================================================
// FreqCorrection Tests
//==============================================================================

TEST(FreqCorrectionTest, DefaultConstruction) {
    FreqCorrection corrector;

    EXPECT_EQ(corrector.get_config().sample_rate_hz, 6.25e6);
    EXPECT_EQ(corrector.get_total_cfo(), 0.0);
    EXPECT_EQ(corrector.get_phase(), 0.0);
    EXPECT_EQ(corrector.get_sample_count(), 0u);
}

TEST(FreqCorrectionTest, CustomConfig) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 12.5e6;
    config.initial_cfo_hz = 500.0;
    config.enable_fine_tracking = false;

    FreqCorrection corrector(config);

    EXPECT_EQ(corrector.get_config().sample_rate_hz, 12.5e6);
    EXPECT_NEAR(corrector.get_total_cfo(), 500.0, 1e-10);
}

TEST(FreqCorrectionTest, SetCoarseCfo) {
    FreqCorrection corrector;

    corrector.set_coarse_cfo(250.0);
    EXPECT_NEAR(corrector.get_total_cfo(), 250.0, 1e-10);

    corrector.set_coarse_cfo(-100.0);
    EXPECT_NEAR(corrector.get_total_cfo(), -100.0, 1e-10);
}

TEST(FreqCorrectionTest, ZeroCfoPassthrough) {
    FreqCorrection corrector;

    // With zero CFO, output should equal input
    std::vector<sample_t> in(100);
    std::vector<sample_t> out(100);

    for (size_t i = 0; i < 100; ++i) {
#ifdef ATSC3_FIXED_POINT
        in[i] = sample_t(float_to_q15(0.5f), float_to_q15(0.25f));
#else
        in[i] = sample_t(0.5f, 0.25f);
#endif
    }

    corrector.process(in.data(), out.data(), 100);

    // Should be essentially unchanged
    for (size_t i = 0; i < 100; ++i) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(out[i].real()), 0.5f, 0.001f);
        EXPECT_NEAR(q15_to_float(out[i].imag()), 0.25f, 0.001f);
#else
        EXPECT_NEAR(out[i].real(), 0.5f, 1e-6f);
        EXPECT_NEAR(out[i].imag(), 0.25f, 1e-6f);
#endif
    }
}

TEST(FreqCorrectionTest, CfoAppliesRotation) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 1000.0;  // Simple sample rate for testing
    config.initial_cfo_hz = 250.0;   // 250 Hz = 0.25 cycles per sample = 90° per sample

    FreqCorrection corrector(config);

    // Start with unit real sample
#ifdef ATSC3_FIXED_POINT
    // Use 32767 (max Q15) to avoid overflow from float_to_q15(1.0f)
    sample_t in = sample_t(32767, 0);
#else
    sample_t in = sample_t(1.0f, 0.0f);
#endif

    sample_t out;
    corrector.process(&in, &out, 1);

    // First sample should be unchanged (phase starts at 0)
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(out.real()), 1.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(out.imag()), 0.0f, 0.01f);
#else
    EXPECT_NEAR(out.real(), 1.0f, 1e-5f);
    EXPECT_NEAR(out.imag(), 0.0f, 1e-5f);
#endif

    // After one sample, phase should have advanced
    corrector.process(&in, &out, 1);

    // Phase increment = -2*pi*250/1000 = -pi/2 per sample
    // After one sample: rotation by -90 degrees
    // (1+0j) * exp(-j*pi/2) = (1+0j) * (0 - j) = -j = (0, -1)
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(out.real()), 0.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(out.imag()), -1.0f, 0.01f);
#else
    EXPECT_NEAR(out.real(), 0.0f, 1e-5f);
    EXPECT_NEAR(out.imag(), -1.0f, 1e-5f);
#endif
}

TEST(FreqCorrectionTest, CfoCorrectionRemovesFrequency) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 1000.0;
    config.initial_cfo_hz = 100.0;  // Correct for 100 Hz offset

    FreqCorrection corrector(config);

    // Generate input with +100 Hz offset (to be corrected to DC)
    // Use amplitude 0.9 to stay within Q1.15 representable range
    constexpr float kAmplitude = 0.9f;
    size_t n = 100;
    std::vector<sample_t> in(n);
    std::vector<sample_t> out(n);

    double freq_hz = 100.0;
    double phase_per_sample = 2.0 * kPi * freq_hz / config.sample_rate_hz;

    for (size_t i = 0; i < n; ++i) {
        double phase = phase_per_sample * static_cast<double>(i);
        float re = kAmplitude * static_cast<float>(std::cos(phase));
        float im = kAmplitude * static_cast<float>(std::sin(phase));
#ifdef ATSC3_FIXED_POINT
        in[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        in[i] = sample_t(re, im);
#endif
    }

    corrector.process(in.data(), out.data(), n);

    // Output should be approximately DC (constant phase)
    // Check last few samples (after transient)
    for (size_t i = n - 10; i < n; ++i) {
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(out[i].real());
        float im = q15_to_float(out[i].imag());
#else
        float re = out[i].real();
        float im = out[i].imag();
#endif
        // Should be close to (amplitude, 0) - DC at the original amplitude
        EXPECT_NEAR(re, kAmplitude, 0.05f) << "at sample " << i;
        EXPECT_NEAR(im, 0.0f, 0.05f) << "at sample " << i;
    }
}

TEST(FreqCorrectionTest, InPlaceProcessing) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 1000.0;
    config.initial_cfo_hz = 0.0;

    FreqCorrection corrector;

    std::vector<sample_t> data(50);
    for (size_t i = 0; i < 50; ++i) {
#ifdef ATSC3_FIXED_POINT
        data[i] = sample_t(float_to_q15(0.5f), float_to_q15(0.5f));
#else
        data[i] = sample_t(0.5f, 0.5f);
#endif
    }

    corrector.process(data.data(), 50);

    // With zero CFO, data should be unchanged
#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(data[0].real()), 0.5f, 0.001f);
    EXPECT_NEAR(q15_to_float(data[49].real()), 0.5f, 0.001f);
#else
    EXPECT_NEAR(data[0].real(), 0.5f, 1e-6f);
    EXPECT_NEAR(data[49].real(), 0.5f, 1e-6f);
#endif
}

TEST(FreqCorrectionTest, ResetPhase) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 1000.0;
    config.initial_cfo_hz = 100.0;

    FreqCorrection corrector(config);

    // Use 0.9 amplitude to stay within Q1.15 representable range
    std::vector<sample_t> data(100);
#ifdef ATSC3_FIXED_POINT
    std::fill(data.begin(), data.end(), sample_t(float_to_q15(0.9f), float_to_q15(0.0f)));
#else
    std::fill(data.begin(), data.end(), sample_t(0.9f, 0.0f));
#endif

    // Process 37 samples (not a multiple of 10, so phase won't wrap to 0)
    // At 100 Hz CFO / 1000 Hz sample rate, 10 samples = 1 full rotation
    corrector.process(data.data(), 37);
    EXPECT_NE(corrector.get_phase(), 0.0);

    // Reset phase
    corrector.reset_phase();
    EXPECT_EQ(corrector.get_phase(), 0.0);
}

TEST(FreqCorrectionTest, SampleCountIncreases) {
    FreqCorrection corrector;

    std::vector<sample_t> data(100);
#ifdef ATSC3_FIXED_POINT
    std::fill(data.begin(), data.end(), sample_t(float_to_q15(0.5f), float_to_q15(0.0f)));
#else
    std::fill(data.begin(), data.end(), sample_t(0.5f, 0.0f));
#endif

    EXPECT_EQ(corrector.get_sample_count(), 0u);

    corrector.process(data.data(), 50);
    EXPECT_EQ(corrector.get_sample_count(), 50u);

    corrector.process(data.data(), 50);
    EXPECT_EQ(corrector.get_sample_count(), 100u);
}

TEST(FreqCorrectionTest, NegativeCfo) {
    FreqCorrectionConfig config;
    config.sample_rate_hz = 1000.0;
    config.initial_cfo_hz = -100.0;

    FreqCorrection corrector(config);

    EXPECT_NEAR(corrector.get_total_cfo(), -100.0, 1e-10);

    // Phase should rotate in opposite direction
#ifdef ATSC3_FIXED_POINT
    // Use 32767 (max Q15) to avoid overflow from float_to_q15(1.0f)
    sample_t in = sample_t(32767, 0);
#else
    sample_t in = sample_t(1.0f, 0.0f);
#endif
    sample_t out;

    // Skip first sample
    corrector.process(&in, &out, 1);

    // After one sample with -100 Hz CFO and 1000 Hz sample rate:
    // phase_increment = -2*pi*(-100)/1000 = +pi/5
    // After second sample: rotation by +pi/5 = 36 degrees
    corrector.process(&in, &out, 1);

#ifdef ATSC3_FIXED_POINT
    float re = q15_to_float(out.real());
    float im = q15_to_float(out.imag());
#else
    float re = out.real();
    float im = out.imag();
#endif

    // cos(pi/5) ≈ 0.809, sin(pi/5) ≈ 0.588
    EXPECT_NEAR(re, 0.809f, 0.05f);
    EXPECT_NEAR(im, 0.588f, 0.05f);
}

TEST(FreqCorrectionTest, CorrectionOf500HzCfo) {
    // ATSC 3.0 requirement: ±500 Hz CFO corrected to < 10 Hz residual
    // Test with realistic sample rate (6.25 MHz)
    FreqCorrectionConfig config;
    config.sample_rate_hz = 6.25e6;
    config.initial_cfo_hz = 500.0;  // 500 Hz CFO to correct

    FreqCorrection corrector(config);

    // Generate input with +500 Hz offset (to be corrected to DC)
    constexpr float kAmplitude = 0.9f;
    size_t n = 10000;  // Enough samples for accurate measurement
    std::vector<sample_t> in(n);
    std::vector<sample_t> out(n);

    double freq_hz = 500.0;
    double phase_per_sample = 2.0 * kPi * freq_hz / config.sample_rate_hz;

    for (size_t i = 0; i < n; ++i) {
        double phase = phase_per_sample * static_cast<double>(i);
        float re = kAmplitude * static_cast<float>(std::cos(phase));
        float im = kAmplitude * static_cast<float>(std::sin(phase));
#ifdef ATSC3_FIXED_POINT
        in[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        in[i] = sample_t(re, im);
#endif
    }

    corrector.process(in.data(), out.data(), n);

    // Measure residual frequency by computing phase change over many samples
    // Take samples from the last part of the output (after any transient)
    size_t start = n - 1000;
    size_t end = n - 1;

    double phase_start, phase_end;
#ifdef ATSC3_FIXED_POINT
    phase_start = std::atan2(q15_to_float(out[start].imag()), q15_to_float(out[start].real()));
    phase_end = std::atan2(q15_to_float(out[end].imag()), q15_to_float(out[end].real()));
#else
    phase_start = std::atan2(out[start].imag(), out[start].real());
    phase_end = std::atan2(out[end].imag(), out[end].real());
#endif

    // Phase change over (end - start) samples
    double phase_change = phase_end - phase_start;
    // Unwrap if needed
    while (phase_change > kPi)
        phase_change -= 2.0 * kPi;
    while (phase_change < -kPi)
        phase_change += 2.0 * kPi;

    // Convert to frequency
    double residual_freq_hz = phase_change / (2.0 * kPi) * config.sample_rate_hz / (end - start);

    // Residual should be < 10 Hz
    EXPECT_LT(std::abs(residual_freq_hz), 10.0)
        << "Residual frequency " << residual_freq_hz << " Hz exceeds 10 Hz limit";
}

TEST(FreqCorrectionTest, CorrectionOfNegative500HzCfo) {
    // Test negative CFO direction
    FreqCorrectionConfig config;
    config.sample_rate_hz = 6.25e6;
    config.initial_cfo_hz = -500.0;

    FreqCorrection corrector(config);

    constexpr float kAmplitude = 0.9f;
    size_t n = 10000;
    std::vector<sample_t> in(n);
    std::vector<sample_t> out(n);

    double freq_hz = -500.0;
    double phase_per_sample = 2.0 * kPi * freq_hz / config.sample_rate_hz;

    for (size_t i = 0; i < n; ++i) {
        double phase = phase_per_sample * static_cast<double>(i);
        float re = kAmplitude * static_cast<float>(std::cos(phase));
        float im = kAmplitude * static_cast<float>(std::sin(phase));
#ifdef ATSC3_FIXED_POINT
        in[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        in[i] = sample_t(re, im);
#endif
    }

    corrector.process(in.data(), out.data(), n);

    size_t start = n - 1000;
    size_t end = n - 1;

    double phase_start, phase_end;
#ifdef ATSC3_FIXED_POINT
    phase_start = std::atan2(q15_to_float(out[start].imag()), q15_to_float(out[start].real()));
    phase_end = std::atan2(q15_to_float(out[end].imag()), q15_to_float(out[end].real()));
#else
    phase_start = std::atan2(out[start].imag(), out[start].real());
    phase_end = std::atan2(out[end].imag(), out[end].real());
#endif

    double phase_change = phase_end - phase_start;
    while (phase_change > kPi)
        phase_change -= 2.0 * kPi;
    while (phase_change < -kPi)
        phase_change += 2.0 * kPi;

    double residual_freq_hz = phase_change / (2.0 * kPi) * config.sample_rate_hz / (end - start);

    EXPECT_LT(std::abs(residual_freq_hz), 10.0)
        << "Residual frequency " << residual_freq_hz << " Hz exceeds 10 Hz limit";
}

//==============================================================================
// PilotPhaseTracker Tests
//==============================================================================

TEST(PilotPhaseTrackerTest, DefaultConstruction) {
    PilotPhaseTracker tracker;

    EXPECT_EQ(tracker.get_symbol_count(), 0u);
    EXPECT_EQ(tracker.get_phase_error_per_sample(), 0.0);
}

TEST(PilotPhaseTrackerTest, CustomConfig) {
    PilotPhaseTrackerConfig config;
    config.fft_size = 16384;
    config.averaging_window = 8;

    PilotPhaseTracker tracker(config);

    EXPECT_EQ(tracker.get_symbol_count(), 0u);
}

TEST(PilotPhaseTrackerTest, Reset) {
    PilotPhaseTracker tracker;

    // Feed some pilot data
    std::vector<double> phases = {0.1, 0.2, 0.3, 0.4};
    std::vector<size_t> indices = {100, 200, 300, 400};

    tracker.update(phases.data(), indices.data(), phases.size());
    EXPECT_GT(tracker.get_symbol_count(), 0u);

    tracker.reset();
    EXPECT_EQ(tracker.get_symbol_count(), 0u);
}

TEST(PilotPhaseTrackerTest, LinearPhaseSlope) {
    PilotPhaseTrackerConfig config;
    config.fft_size = 8192;
    config.averaging_window = 1;  // No averaging

    PilotPhaseTracker tracker(config);

    // Create pilots with known linear phase slope
    // phase[k] = a + slope * k
    double slope = 0.001;  // radians per subcarrier
    std::vector<double> phases;
    std::vector<size_t> indices;

    for (size_t k = 0; k < 10; ++k) {
        size_t idx = 100 + k * 100;
        double phase = slope * static_cast<double>(idx);
        indices.push_back(idx);
        phases.push_back(phase);
    }

    tracker.update(phases.data(), indices.data(), phases.size());

    // Should detect the slope
    EXPECT_NEAR(tracker.get_phase_error_per_sample(), slope, slope * 0.1);
}

TEST(PilotPhaseTrackerTest, NeedsTwoPilots) {
    PilotPhaseTracker tracker;

    // Single pilot - should not update
    double phases[] = {0.5};
    size_t indices[] = {100};

    tracker.update(phases, indices, 1);

    // Should not have updated
    EXPECT_EQ(tracker.get_phase_error_per_sample(), 0.0);
}

TEST(PilotPhaseTrackerTest, SymbolCountIncreases) {
    PilotPhaseTracker tracker;

    std::vector<double> phases = {0.1, 0.2, 0.3};
    std::vector<size_t> indices = {100, 200, 300};

    EXPECT_EQ(tracker.get_symbol_count(), 0u);

    tracker.update(phases.data(), indices.data(), phases.size());
    EXPECT_EQ(tracker.get_symbol_count(), 1u);

    tracker.update(phases.data(), indices.data(), phases.size());
    EXPECT_EQ(tracker.get_symbol_count(), 2u);
}

TEST(PilotPhaseTrackerTest, ResidualCfoConversion) {
    PilotPhaseTrackerConfig config;
    config.fft_size = 8192;
    config.averaging_window = 1;

    PilotPhaseTracker tracker(config);

    // Set up a known phase error per sample
    double slope = 0.001;
    std::vector<double> phases;
    std::vector<size_t> indices;

    for (size_t k = 0; k < 5; ++k) {
        size_t idx = 100 + k * 100;
        double phase = slope * static_cast<double>(idx);
        indices.push_back(idx);
        phases.push_back(phase);
    }

    tracker.update(phases.data(), indices.data(), phases.size());

    // Test conversion to Hz
    double sample_rate = 6.25e6;
    double cfo_hz = tracker.get_residual_cfo(sample_rate);

    // Should be slope * Fs / (2*pi)
    double expected_cfo = slope * sample_rate / (2.0 * kPi);
    EXPECT_NEAR(cfo_hz, expected_cfo, expected_cfo * 0.1);
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
