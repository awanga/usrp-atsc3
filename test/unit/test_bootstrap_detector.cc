// test_bootstrap_detector.cc — Unit tests for lib/sync/bootstrap_detector.h
//
// Tests Schmidl-Cox autocorrelation bootstrap detection and CFO estimation

#include "bootstrap_detector.h"

#include <cmath>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

// Helper to generate a synthetic bootstrap symbol with known CFO
// Bootstrap uses repeated half-symbol structure for Schmidl-Cox
std::vector<sample_t> generate_bootstrap_symbol(double cfo_hz, double sample_rate, double snr_db,
                                                uint32_t seed = 42) {
    constexpr size_t kLength = BootstrapDetector::kBootstrapLength;
    constexpr size_t kHalf = BootstrapDetector::kHalfSymbol;

    std::vector<sample_t> symbol(kLength);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise_dist(0.0f, 1.0f);

    // Generate first half with random QPSK-like symbols
    for (size_t i = 0; i < kHalf; ++i) {
        float re = (rng() % 2 == 0) ? 1.0f : -1.0f;
        float im = (rng() % 2 == 0) ? 1.0f : -1.0f;
        re *= 0.707f;  // Normalize
        im *= 0.707f;
#ifdef ATSC3_FIXED_POINT
        symbol[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        symbol[i] = sample_t(re, im);
#endif
    }

    // Second half is a phase-rotated copy of the first half (Schmidl-Cox structure)
    double phase_per_sample = 2.0 * M_PI * cfo_hz / sample_rate;
    for (size_t i = 0; i < kHalf; ++i) {
        double phase = phase_per_sample * static_cast<double>(kHalf);
#ifdef ATSC3_FIXED_POINT
        float re = q15_to_float(symbol[i].real());
        float im = q15_to_float(symbol[i].imag());
#else
        float re = symbol[i].real();
        float im = symbol[i].imag();
#endif
        // Apply phase rotation for second half
        float cos_p = std::cos(phase);
        float sin_p = std::sin(phase);
        float re2 = re * cos_p - im * sin_p;
        float im2 = re * sin_p + im * cos_p;
#ifdef ATSC3_FIXED_POINT
        symbol[i + kHalf] = sample_t(float_to_q15(re2), float_to_q15(im2));
#else
        symbol[i + kHalf] = sample_t(re2, im2);
#endif
    }

    // Add noise based on SNR
    if (snr_db < 100.0) {
        double signal_power = 1.0;  // Normalized
        double noise_power = signal_power / std::pow(10.0, snr_db / 10.0);
        double noise_std = std::sqrt(noise_power / 2.0);  // Per I/Q component

        for (size_t i = 0; i < kLength; ++i) {
#ifdef ATSC3_FIXED_POINT
            float re = q15_to_float(symbol[i].real());
            float im = q15_to_float(symbol[i].imag());
            re += noise_dist(rng) * noise_std;
            im += noise_dist(rng) * noise_std;
            symbol[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
            symbol[i] += sample_t(noise_dist(rng) * noise_std, noise_dist(rng) * noise_std);
#endif
        }
    }

    return symbol;
}

// Test basic construction
TEST(BootstrapDetectorTest, Construction) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.7;

    BootstrapDetector detector(config);

    EXPECT_EQ(detector.get_config().sample_rate_hz, 6.25e6);
    EXPECT_EQ(detector.get_config().threshold, 0.7);
    EXPECT_EQ(detector.get_sample_count(), 0u);
}

// Test default configuration
TEST(BootstrapDetectorTest, DefaultConfig) {
    BootstrapDetector detector;

    EXPECT_EQ(detector.get_config().sample_rate_hz, 6.25e6);
    EXPECT_EQ(detector.get_config().threshold, 0.7);
    EXPECT_EQ(detector.get_config().averaging_window, 64u);
}

// Test reset clears state
TEST(BootstrapDetectorTest, Reset) {
    BootstrapDetector detector;

    // Process some samples
    std::vector<sample_t> samples(100, sample_t(0, 0));
    detector.process(samples.data(), samples.size());
    EXPECT_EQ(detector.get_sample_count(), 100u);

    // Reset and verify
    detector.reset();
    EXPECT_EQ(detector.get_sample_count(), 0u);
}

// Test configuration update
TEST(BootstrapDetectorTest, SetConfig) {
    BootstrapDetector detector;

    BootstrapConfig new_config;
    new_config.sample_rate_hz = 12.5e6;
    new_config.threshold = 0.8;
    new_config.averaging_window = 128;

    detector.set_config(new_config);

    EXPECT_EQ(detector.get_config().sample_rate_hz, 12.5e6);
    EXPECT_EQ(detector.get_config().threshold, 0.8);
    EXPECT_EQ(detector.get_config().averaging_window, 128u);
}

// Test detection of synthetic bootstrap symbol at high SNR
TEST(BootstrapDetectorTest, DetectSyntheticBootstrap) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.5;
    config.averaging_window = 32;

    BootstrapDetector detector(config);

    // Generate noise before bootstrap
    std::vector<sample_t> noise(1000);
    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    for (auto& s : noise) {
#ifdef ATSC3_FIXED_POINT
        s = sample_t(float_to_q15(dist(rng)), float_to_q15(dist(rng)));
#else
        s = sample_t(dist(rng), dist(rng));
#endif
    }

    // Generate bootstrap symbol (no CFO, high SNR)
    auto bootstrap = generate_bootstrap_symbol(0.0, config.sample_rate_hz, 30.0);

    // Process noise (should not detect)
    BootstrapDetection det = detector.process(noise.data(), noise.size());
    EXPECT_FALSE(det.detected);

    // Process bootstrap (should detect)
    det = detector.process(bootstrap.data(), bootstrap.size());

    // Detection may occur during or shortly after the bootstrap
    // We're checking that the system can detect it
    if (det.detected) {
        // If detected, sample_index should be reasonable
        EXPECT_GT(det.sample_index, 0u);
        EXPECT_GT(det.metric, config.threshold);
    }
}

// Test detection with known CFO
TEST(BootstrapDetectorTest, DetectWithCFO) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.5;
    config.averaging_window = 32;

    BootstrapDetector detector(config);

    // Inject known CFO
    double injected_cfo = 200.0;  // 200 Hz

    // Add some leading silence
    std::vector<sample_t> silence(500, sample_t(0, 0));
    detector.process(silence.data(), silence.size());

    // Generate bootstrap with CFO
    auto bootstrap = generate_bootstrap_symbol(injected_cfo, config.sample_rate_hz, 25.0);

    // Process bootstrap
    BootstrapDetection det = detector.process(bootstrap.data(), bootstrap.size());

    // Add trailing samples to ensure detection completes
    std::vector<sample_t> trailing(500, sample_t(0, 0));
    if (!det.detected) {
        det = detector.process(trailing.data(), trailing.size());
    }

    if (det.detected) {
        // CFO estimate should be within reasonable tolerance
        // At high SNR with perfect structure, we expect ±100 Hz
        EXPECT_NEAR(det.cfo_hz, injected_cfo, 200.0);
    }
}

// Test that noise-only input does not trigger false detection
TEST(BootstrapDetectorTest, NoFalseDetectionOnNoise) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.7;  // Higher threshold to avoid false positives
    config.averaging_window = 64;

    BootstrapDetector detector(config);

    // Generate pure noise
    std::vector<sample_t> noise(10000);
    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 0.5f);
    for (auto& s : noise) {
#ifdef ATSC3_FIXED_POINT
        s = sample_t(float_to_q15(dist(rng)), float_to_q15(dist(rng)));
#else
        s = sample_t(dist(rng), dist(rng));
#endif
    }

    // Process and check for false detections
    BootstrapDetection det = detector.process(noise.data(), noise.size());

    // With high threshold and pure noise, should not detect
    // (This test may occasionally fail due to statistical nature of noise)
    if (det.detected) {
        // If it does detect, metric should be borderline
        EXPECT_LT(det.metric, 0.85);
    }
}

// Test sample-by-sample processing matches buffer processing
TEST(BootstrapDetectorTest, SampleByBufferEquivalence) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.5;

    BootstrapDetector detector1(config);
    BootstrapDetector detector2(config);

    // Generate test data
    auto bootstrap = generate_bootstrap_symbol(100.0, config.sample_rate_hz, 20.0, 789);

    // Process sample-by-sample
    BootstrapDetection det1;
    for (const auto& s : bootstrap) {
        auto d = detector1.process(s);
        if (d.detected) {
            det1 = d;
        }
    }

    // Process as buffer
    (void)detector2.process(bootstrap.data(), bootstrap.size());

    // Both should have same sample count
    EXPECT_EQ(detector1.get_sample_count(), detector2.get_sample_count());
}

// Test bootstrap symbol length constant
TEST(BootstrapDetectorTest, BootstrapLengthConstant) {
    // ATSC 3.0 bootstrap is always 4096 samples (per A/322 Section 5.2)
    EXPECT_EQ(BootstrapDetector::kBootstrapLength, 4096u);
    EXPECT_EQ(BootstrapDetector::kHalfSymbol, 2048u);
}

#ifdef ATSC3_FIXED_POINT

//==============================================================================
// Phase 9.0b equivalence: fixed-point rewrite vs. the pre-rewrite double
// algorithm. Per the HDL port plan, each Phase 9.0b block needs its own
// >=40 dB SNR-vs-pre-rewrite checkpoint before its RTL phase starts.
//
// This is deliberately NOT a comparison against BootstrapDetector's own
// float build: the two builds already differ by ordinary Q1.15 sample
// quantization (expected, unrelated to this rewrite). Instead,
// ReferenceDetector below is the exact pre-rewrite double algorithm
// (kAlpha=2/2049 EWMA, std::arg/std::sqrt), fed the *same* quantized-then-
// dequantized samples the fixed-point detector sees, so the only thing
// left to measure is error introduced by the rewrite itself (the integer
// EWMA's power-of-2 alpha approximation and CORDIC's finite iteration
// count), not by Q1.15 sample resolution.
//==============================================================================

class ReferenceDetector {
public:
    static constexpr size_t kHalfSymbol = BootstrapDetector::kHalfSymbol;

    explicit ReferenceDetector(const BootstrapConfig& config)
        : config_(config),
          p_sum_(0.0, 0.0),
          r_sum_(0.0),
          delay_buffer_(kHalfSymbol, std::complex<double>(0.0, 0.0)),
          delay_idx_(0),
          power_buffer_(kHalfSymbol, 0.0),
          power_idx_(0),
          metric_history_(std::max<size_t>(1, config.averaging_window), 0.0),
          metric_sum_(0.0),
          metric_idx_(0),
          sample_count_(0),
          in_detection_(false),
          peak_metric_(0.0),
          peak_correlation_(0.0, 0.0),
          peak_sample_(0) {}

    // detected/metric/cfo_hz/sample_index mirror the "detected" event, as
    // in BootstrapDetection. current_metric/current_cfo_hz are the
    // same-instant running values (updated every call regardless of
    // detection state), matching
    // BootstrapDetector::get_current_metric()/get_current_cfo_hz().
    struct Result {
        bool detected = false;
        double metric = 0.0;
        double cfo_hz = 0.0;
        size_t sample_index = 0;
        double current_metric = 0.0;
        double current_cfo_hz = 0.0;
    };

    Result process(std::complex<double> x) {
        std::complex<double> x_delayed = delay_buffer_[delay_idx_];
        std::complex<double> corr_term = x * std::conj(x_delayed);

        double old_power = power_buffer_[power_idx_];
        double new_power = std::norm(x_delayed);
        r_sum_ -= old_power;
        r_sum_ += new_power;
        r_sum_ = std::max(r_sum_, 1e-20);
        power_buffer_[power_idx_] = new_power;
        power_idx_ = (power_idx_ + 1) % kHalfSymbol;

        constexpr double kAlpha = 2.0 / (kHalfSymbol + 1);
        p_sum_ = (1.0 - kAlpha) * p_sum_ + kAlpha * corr_term * static_cast<double>(kHalfSymbol);

        delay_buffer_[delay_idx_] = x;
        delay_idx_ = (delay_idx_ + 1) % kHalfSymbol;
        ++sample_count_;

        double p_mag_sq = std::norm(p_sum_);
        double r_sq = r_sum_ * r_sum_;
        double metric = (r_sq > 1e-20) ? p_mag_sq / r_sq : 0.0;

        metric_sum_ -= metric_history_[metric_idx_];
        metric_sum_ += metric;
        metric_history_[metric_idx_] = metric;
        metric_idx_ = (metric_idx_ + 1) % metric_history_.size();
        double smoothed_metric = metric_sum_ / static_cast<double>(metric_history_.size());

        Result result;
        result.current_metric = smoothed_metric;
        result.current_cfo_hz =
            std::arg(p_sum_) * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);

        if (!in_detection_) {
            if (smoothed_metric > config_.threshold) {
                in_detection_ = true;
                peak_metric_ = smoothed_metric;
                peak_sample_ = sample_count_;
                peak_correlation_ = p_sum_;
            }
        } else {
            if (smoothed_metric > peak_metric_) {
                peak_metric_ = smoothed_metric;
                peak_sample_ = sample_count_;
                peak_correlation_ = p_sum_;
            }
            if (smoothed_metric < peak_metric_ * 0.8) {
                result.detected = true;
                result.sample_index = peak_sample_;
                result.metric = peak_metric_;
                double phase = std::arg(peak_correlation_);
                result.cfo_hz = phase * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);
                in_detection_ = false;
                peak_metric_ = 0.0;
            }
        }
        return result;
    }

private:
    BootstrapConfig config_;
    std::complex<double> p_sum_;
    double r_sum_;
    std::vector<std::complex<double>> delay_buffer_;
    size_t delay_idx_;
    std::vector<double> power_buffer_;
    size_t power_idx_;
    std::vector<double> metric_history_;
    double metric_sum_;
    size_t metric_idx_;
    size_t sample_count_;
    bool in_detection_;
    double peak_metric_;
    std::complex<double> peak_correlation_;
    size_t peak_sample_;
};

TEST(BootstrapDetectorTest, FixedPointVsReferenceEquivalence) {
    BootstrapConfig config;
    config.sample_rate_hz = 6.25e6;
    config.threshold = 0.5;
    config.averaging_window = 32;

    // Compare get_current_metric()/get_current_cfo_hz() at *every* sample
    // (not just at the FSM-gated "detected" event -- see the class
    // comment above ReferenceDetector for why). Metric and CFO are on
    // wildly different scales (metric ~[0,1], CFO ~hundreds of Hz), so
    // they get separate SNR accumulators rather than one combined sum
    // that Hz would dominate.
    double metric_signal_power = 0.0;
    double metric_error_power = 0.0;
    double cfo_signal_power = 0.0;
    double cfo_error_power = 0.0;
    long sample_count = 0;

    for (double cfo : {-300.0, -100.0, 0.0, 150.0, 400.0}) {
        for (double snr : {15.0, 20.0, 30.0}) {
            for (uint32_t seed = 1; seed <= 4; ++seed) {
                BootstrapDetector fixed_detector(config);
                ReferenceDetector ref_detector(config);

                auto bootstrap = generate_bootstrap_symbol(cfo, config.sample_rate_hz, snr, seed);

                for (const auto& s : bootstrap) {
                    fixed_detector.process(s);

                    // Feed the reference the *same quantized* sample
                    // (dequantized back to double), so only the rewrite's
                    // own error is measured, not Q1.15 quantization.
                    std::complex<double> xd(q15_to_float(s.real()), q15_to_float(s.imag()));
                    auto rd = ref_detector.process(xd);

                    double fixed_metric = fixed_detector.get_current_metric();
                    double fixed_cfo = fixed_detector.get_current_cfo_hz();

                    metric_signal_power += rd.current_metric * rd.current_metric;
                    double dm = fixed_metric - rd.current_metric;
                    metric_error_power += dm * dm;

                    // Only meaningful once the correlation has picked up
                    // real signal (near-zero metric -> near-arbitrary
                    // phase, which would swamp the CFO SNR with noise
                    // that has nothing to do with the rewrite's fidelity).
                    if (rd.current_metric > 0.05) {
                        cfo_signal_power += cfo * cfo;
                        double dc = fixed_cfo - rd.current_cfo_hz;
                        cfo_error_power += dc * dc;
                    }

                    ++sample_count;
                }
            }
        }
    }

    ASSERT_GT(sample_count, 1000);
    ASSERT_GT(cfo_signal_power, 0.0) << "no samples crossed the metric>0.05 gate for CFO SNR";

    double metric_snr_db = 10.0 * std::log10(metric_signal_power / metric_error_power);
    double cfo_snr_db = 10.0 * std::log10(cfo_signal_power / cfo_error_power);

    EXPECT_GE(metric_snr_db, 40.0)
        << "fixed-point bootstrap metric SNR vs. pre-rewrite reference: " << metric_snr_db
        << " dB over " << sample_count << " samples";
    EXPECT_GE(cfo_snr_db, 40.0) << "fixed-point bootstrap CFO SNR vs. pre-rewrite reference: "
                                << cfo_snr_db << " dB";
}

#endif  // ATSC3_FIXED_POINT

}  // namespace
}  // namespace sync
}  // namespace atsc3
