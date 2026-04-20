// test_fft_engine.cc — Unit tests for lib/ofdm/fft_engine.h
//
// Tests FFT correctness, FFTW/fixed-point backends, and wisdom caching

#include <gtest/gtest.h>

#include "fft_engine.h"

#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Helper to compute expected FFT bin for a complex sinusoid
// For a sinusoid at frequency f with N-point FFT:
// bin = f * N / sample_rate
[[maybe_unused]]
size_t expected_bin(double freq_hz, double sample_rate, size_t fft_size) {
    double bin = freq_hz * static_cast<double>(fft_size) / sample_rate;
    return static_cast<size_t>(std::round(bin)) % fft_size;
}

// Helper to generate complex sinusoid
[[maybe_unused]]
std::vector<std::complex<float>> generate_sinusoid(size_t n, double freq_hz,
                                                    double sample_rate,
                                                    double amplitude = 1.0) {
    std::vector<std::complex<float>> signal(n);
    double phase_step = 2.0 * kPi * freq_hz / sample_rate;

    for (size_t i = 0; i < n; ++i) {
        double phase = phase_step * static_cast<double>(i);
        signal[i] = std::complex<float>(amplitude * std::cos(phase),
                                        amplitude * std::sin(phase));
    }
    return signal;
}

// Helper to find peak bin in FFT output
[[maybe_unused]]
size_t find_peak_bin(const std::complex<float>* fft_out, size_t n) {
    size_t peak_bin = 0;
    float peak_mag = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        float mag = std::abs(fft_out[i]);
        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = i;
        }
    }
    return peak_bin;
}

// Test factory creates an engine
TEST(FftEngineTest, FactoryCreation) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);

    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->get_size(), 8192u);
    EXPECT_EQ(engine->get_direction(), FftDirection::kForward);
}

// Test all supported FFT sizes
TEST(FftEngineTest, SupportedSizes) {
    std::vector<FftSize> sizes = {FftSize::k4K, FftSize::k8K, FftSize::k16K, FftSize::k32K};

    for (FftSize size : sizes) {
        FftConfig config;
        config.size = size;

        auto engine = FftEngine::create(config);
        ASSERT_NE(engine, nullptr) << "Failed to create engine for size "
                                   << static_cast<size_t>(size);
        EXPECT_EQ(engine->get_size(), static_cast<size_t>(size));
    }
}

// Test forward FFT of DC signal (all ones)
// Note: This test uses values at the edge of Q15 range, so only valid in float mode
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, ForwardFftDcSignal) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    size_t n = engine->get_size();

    // All-ones input (DC)
    std::vector<std::complex<float>> in(n, std::complex<float>(1.0f, 0.0f));
    std::vector<std::complex<float>> out(n);

    engine->process(in.data(), out.data());

    // DC component should be at bin 0, with magnitude = N
    EXPECT_NEAR(std::abs(out[0]), static_cast<float>(n), 1.0f);

    // All other bins should be near zero
    for (size_t i = 1; i < n; ++i) {
        EXPECT_LT(std::abs(out[i]), 1.0f)
            << "Non-zero energy in bin " << i << " for DC input";
    }
}
#endif  // !ATSC3_FIXED_POINT

// Test forward FFT of complex sinusoid
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, ForwardFftSinusoid) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    size_t n = engine->get_size();

    // Generate sinusoid at bin 100
    double sample_rate = 6.25e6;
    double freq_hz = 100.0 * sample_rate / static_cast<double>(n);
    auto in = generate_sinusoid(n, freq_hz, sample_rate);

    std::vector<std::complex<float>> out(n);
    engine->process(in.data(), out.data());

    // Find peak
    size_t peak = find_peak_bin(out.data(), n);
    EXPECT_EQ(peak, 100u) << "Peak at bin " << peak << ", expected 100";

    // Peak magnitude should be approximately N
    EXPECT_NEAR(std::abs(out[peak]), static_cast<float>(n), n * 0.01f);
}
#endif  // !ATSC3_FIXED_POINT

// Test inverse FFT reconstruction
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, InverseFftReconstruction) {
    FftConfig fwd_config;
    fwd_config.size = FftSize::k8K;
    fwd_config.direction = FftDirection::kForward;

    FftConfig inv_config;
    inv_config.size = FftSize::k8K;
    inv_config.direction = FftDirection::kInverse;
    inv_config.normalize_inverse = true;

    auto fwd_engine = FftEngine::create(fwd_config);
    auto inv_engine = FftEngine::create(inv_config);
    size_t n = fwd_engine->get_size();

    // Generate test signal
    double sample_rate = 6.25e6;
    double freq_hz = 500.0 * sample_rate / static_cast<double>(n);
    auto original = generate_sinusoid(n, freq_hz, sample_rate, 0.5);

    // Forward FFT
    std::vector<std::complex<float>> freq_domain(n);
    fwd_engine->process(original.data(), freq_domain.data());

    // Inverse FFT
    std::vector<std::complex<float>> reconstructed(n);
    inv_engine->process(freq_domain.data(), reconstructed.data());

    // Verify reconstruction matches original
    double max_error = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double error = std::abs(reconstructed[i] - original[i]);
        max_error = std::max(max_error, error);
    }

    // With float precision and normalized inverse, error should be very small
    EXPECT_LT(max_error, 1e-4) << "Reconstruction error too large: " << max_error;
}
#endif  // !ATSC3_FIXED_POINT

// Test FFT with sample_t interface
// Note: In fixed-point mode, large FFT sizes cause overflow with DC input
// This test just verifies compilation and basic execution
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, SampleTInterface) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    size_t n = engine->get_size();

    // Create input using sample_t
    std::vector<sample_t> in(n);
    std::vector<sample_t> out(n);

    for (size_t i = 0; i < n; ++i) {
        in[i] = sample_t(0.5f, 0.0f);
    }

    // This should compile and run without error
    engine->process(in.data(), out.data());

    // Basic sanity check - DC component should be largest
    float dc_mag = std::abs(out[0]);
    EXPECT_GT(dc_mag, 0.0f);
}
#endif

// Test 4K FFT (bootstrap size)
// Note: Uses 1.0f which overflows Q15 format
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, Fft4KBootstrap) {
    FftConfig config;
    config.size = FftSize::k4K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    EXPECT_EQ(engine->get_size(), 4096u);

    // Simple functionality test
    std::vector<std::complex<float>> in(4096, std::complex<float>(1.0f, 0.0f));
    std::vector<std::complex<float>> out(4096);

    engine->process(in.data(), out.data());

    // DC bin should have energy
    EXPECT_GT(std::abs(out[0]), 0.0f);
}
#endif

// Test normalization option for inverse FFT
TEST(FftEngineTest, InverseNormalization) {
    // Without normalization
    FftConfig config_no_norm;
    config_no_norm.size = FftSize::k8K;
    config_no_norm.direction = FftDirection::kInverse;
    config_no_norm.normalize_inverse = false;

    // With normalization
    FftConfig config_norm;
    config_norm.size = FftSize::k8K;
    config_norm.direction = FftDirection::kInverse;
    config_norm.normalize_inverse = true;

    auto engine_no_norm = FftEngine::create(config_no_norm);
    auto engine_norm = FftEngine::create(config_norm);
    size_t n = engine_no_norm->get_size();

    // Single-bin input (impulse in frequency domain)
    std::vector<std::complex<float>> in(n, std::complex<float>(0.0f, 0.0f));
    in[0] = std::complex<float>(1.0f, 0.0f);

    std::vector<std::complex<float>> out_no_norm(n);
    std::vector<std::complex<float>> out_norm(n);

    engine_no_norm->process(in.data(), out_no_norm.data());
    engine_norm->process(in.data(), out_norm.data());

    // Non-normalized should have magnitude 1.0
    // Normalized should have magnitude 1/N
    EXPECT_NEAR(std::abs(out_no_norm[0]), 1.0f, 0.01f);
    EXPECT_NEAR(std::abs(out_norm[0]), 1.0f / static_cast<float>(n), 1e-6f);
}

// Test backend type reporting
TEST(FftEngineTest, BackendType) {
    FftConfig config;
    config.size = FftSize::k8K;

    auto engine = FftEngine::create(config);

#ifdef ATSC3_FIXED_POINT
    EXPECT_FALSE(engine->is_fftw_backend());
#else
    EXPECT_TRUE(engine->is_fftw_backend());
#endif
}

// Test Parseval's theorem (energy conservation)
// Note: Requires float precision for energy calculations
#ifndef ATSC3_FIXED_POINT
TEST(FftEngineTest, ParsevalsTheorem) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    size_t n = engine->get_size();

    // Generate random-ish signal
    std::vector<std::complex<float>> in(n);
    for (size_t i = 0; i < n; ++i) {
        float phase = 2.0f * static_cast<float>(kPi) * static_cast<float>(i * 7) /
                      static_cast<float>(n);
        in[i] = std::complex<float>(std::cos(phase), std::sin(phase));
    }

    std::vector<std::complex<float>> out(n);
    engine->process(in.data(), out.data());

    // Time-domain energy
    double time_energy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        time_energy += std::norm(in[i]);
    }

    // Frequency-domain energy (scaled by N for DFT)
    double freq_energy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        freq_energy += std::norm(out[i]);
    }
    freq_energy /= static_cast<double>(n);

    // Parseval: sum|x|^2 = (1/N) * sum|X|^2
    EXPECT_NEAR(time_energy, freq_energy, time_energy * 0.001)
        << "Parseval's theorem violated";
}
#endif  // !ATSC3_FIXED_POINT

// Test wisdom functions exist (even if no-op in fixed-point mode)
TEST(FftEngineTest, WisdomFunctions) {
    // These should compile and not crash
    bool loaded = load_fftw_wisdom("/nonexistent/path");
    bool saved = save_fftw_wisdom("/nonexistent/path");

    // In fixed-point mode, these return false
    // In float mode, they return false for invalid paths
#ifdef ATSC3_FIXED_POINT
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(saved);
#else
    EXPECT_FALSE(loaded);  // Path doesn't exist
    // save might fail or succeed depending on permissions
    (void)saved;
#endif
}

#ifndef ATSC3_FIXED_POINT
// Float-mode specific test: verify FFTW produces correct results for known input
TEST(FftEngineTest, FftwKnownResult) {
    FftConfig config;
    config.size = FftSize::k8K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    size_t n = engine->get_size();

    // Impulse at sample 0
    std::vector<std::complex<float>> in(n, std::complex<float>(0.0f, 0.0f));
    in[0] = std::complex<float>(1.0f, 0.0f);

    std::vector<std::complex<float>> out(n);
    engine->process(in.data(), out.data());

    // FFT of impulse is constant (all 1.0)
    for (size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(out[i].real(), 1.0f, 0.001f) << "Bin " << i;
        EXPECT_NEAR(out[i].imag(), 0.0f, 0.001f) << "Bin " << i;
    }
}
#endif

#ifdef ATSC3_FIXED_POINT
// Fixed-point specific test: verify Cooley-Tukey produces reasonable results
TEST(FftEngineTest, FixedPointBasicOperation) {
    FftConfig config;
    // Use 4K to reduce overflow risk (smaller accumulation)
    config.size = FftSize::k4K;
    config.direction = FftDirection::kForward;

    auto engine = FftEngine::create(config);
    EXPECT_FALSE(engine->is_fftw_backend());

    size_t n = engine->get_size();

    // Use small amplitude to prevent overflow during accumulation
    // 0.001 * 4096 * 32768 = ~134k which still overflows, so use even smaller
    // Better approach: test with a single impulse
    std::vector<sample_t> in(n);
    std::fill(in.begin(), in.end(), sample_t(0, 0));
    // Put impulse at first sample - FFT of impulse is constant
    in[0] = sample_t(float_to_q15(0.5f), float_to_q15(0.0f));

    std::vector<sample_t> out(n);
    engine->process(in.data(), out.data());

    // For impulse input, all output bins should have same magnitude
    float first_re = q15_to_float(out[0].real());
    float first_im = q15_to_float(out[0].imag());
    float first_mag = std::sqrt(first_re * first_re + first_im * first_im);

    // Output should be non-zero (the impulse response)
    EXPECT_GT(first_mag, 0.0f) << "FFT output is zero for impulse input";

    // Check a few more bins - should all be similar magnitude
    float mid_re = q15_to_float(out[n / 2].real());
    float mid_im = q15_to_float(out[n / 2].imag());
    float mid_mag = std::sqrt(mid_re * mid_re + mid_im * mid_im);

    // For impulse, all bins should have similar energy
    EXPECT_NEAR(first_mag, mid_mag, first_mag * 0.5f)
        << "FFT of impulse should have uniform magnitude";
}
#endif

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
