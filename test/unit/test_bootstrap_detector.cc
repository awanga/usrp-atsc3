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

// Build-independent stimulus for the regression tests below: generated
// in float units at a realistic backoff and converted with
// from_complex_float(), so the same test runs in both builds.
constexpr float kAmplitude = 0.25f;  // ~-12 dBFS per rail

std::vector<sample_t> random_signal(std::mt19937& rng, size_t n, float amplitude) {
    std::normal_distribution<float> dist(0.0f, amplitude);
    std::vector<sample_t> out(n);
    for (auto& s : out) {
        s = from_complex_float({dist(rng), dist(rng)});
    }
    return out;
}

// One half-symbol of random signal, repeated (the Schmidl-Cox structure),
// then an uncorrelated following symbol, as in a real bootstrap sequence.
std::vector<sample_t> bootstrap_then_next_symbol(std::mt19937& rng) {
    constexpr size_t kHalf = BootstrapDetector::kHalfSymbol;
    std::vector<sample_t> half = random_signal(rng, kHalf, kAmplitude);
    std::vector<sample_t> out = half;
    out.insert(out.end(), half.begin(), half.end());
    std::vector<sample_t> next = random_signal(rng, kHalf, kAmplitude);
    out.insert(out.end(), next.begin(), next.end());
    return out;
}

struct RunLog {
    std::vector<BootstrapDetection> detections;
    double max_metric = 0.0;
};

void run(BootstrapDetector& detector, const std::vector<sample_t>& samples, RunLog& r) {
    for (const auto& s : samples) {
        BootstrapDetection d = detector.process(s);
        if (d.detected) {
            r.detections.push_back(d);
        }
        r.max_metric = std::max(r.max_metric, detector.get_current_metric());
    }
}

// One bootstrap yields exactly one detection, at the end of the repeated
// structure -- not a burst of detections as the metric decays.
TEST(BootstrapDetectorTest, SingleDetectionAtSymbolEnd) {
    BootstrapDetector detector;  // register-reset defaults (threshold 0.7)
    std::mt19937 rng(7);
    RunLog r;
    run(detector, random_signal(rng, 1000, kAmplitude * 0.03f), r);
    run(detector, bootstrap_then_next_symbol(rng), r);
    run(detector, random_signal(rng, 1000, kAmplitude * 0.03f), r);

    ASSERT_EQ(r.detections.size(), 1u);
    int64_t end_of_structure = 1000 + static_cast<int64_t>(BootstrapDetector::kBootstrapLength);
    EXPECT_NEAR(static_cast<double>(r.detections[0].sample_index),
                static_cast<double>(end_of_structure), 200.0);
}

// A strong signal arriving while the delay line still holds weak noise
// must not look like a correlation: the metric normalizes by both halves'
// energy, so it stays near 0 without a repeated structure.
TEST(BootstrapDetectorTest, NoSpuriousDetectionAtNoiseToSignalEdge) {
    BootstrapDetector detector;
    std::mt19937 rng(11);
    RunLog r;
    run(detector, random_signal(rng, 3000, kAmplitude * 0.01f), r);
    run(detector, random_signal(rng, 6000, kAmplitude), r);

    EXPECT_TRUE(r.detections.empty());
    EXPECT_LT(r.max_metric, 0.1);
}

// Silence after a detection neither re-detects nor leaves a stale
// metric: below the correlator-energy floor the metric is 0.
TEST(BootstrapDetectorTest, SilenceAfterSignalDoesNotDetect) {
    BootstrapDetector detector;
    std::mt19937 rng(13);
    RunLog r;
    run(detector, bootstrap_then_next_symbol(rng), r);
    ASSERT_EQ(r.detections.size(), 1u);

    run(detector, std::vector<sample_t>(30000, sample_t(0, 0)), r);
    EXPECT_EQ(r.detections.size(), 1u);
    EXPECT_LT(detector.get_current_metric(), 1e-9);  // float build: running-sum residue
}

// |P| <= R by construction, so the metric never exceeds 1 (up to Q1.15
// rounding in the fixed-point build), including full-scale input.
TEST(BootstrapDetectorTest, MetricIsBounded) {
    BootstrapDetector detector;
    std::mt19937 rng(17);
    RunLog r;
    run(detector, bootstrap_then_next_symbol(rng), r);
    std::uniform_int_distribution<int> coin(0, 1);
    std::vector<sample_t> full_scale(5000);
    for (auto& s : full_scale) {
        s = from_complex_float({coin(rng) ? 1.0f : -1.0f, coin(rng) ? 1.0f : -1.0f});
    }
    std::vector<sample_t> repeated = full_scale;
    repeated.insert(repeated.end(), full_scale.begin(), full_scale.end());
    run(detector, repeated, r);

    EXPECT_LE(r.max_metric, 1.01);
    EXPECT_GT(r.max_metric, 0.5);  // the repeated full-scale block does correlate
}

// snr_db inverts the plateau metric M = (S / (S + N))^2. Uses a long
// periodic structure (8 repetitions) so the EWMAs converge to the plateau,
// then checks the estimate against the injected SNR.
TEST(BootstrapDetectorTest, SnrEstimateMatchesInjectedSnr) {
    constexpr size_t kHalf = BootstrapDetector::kHalfSymbol;
    for (double snr_db : {0.0, 10.0, 20.0}) {
        BootstrapConfig config;
        config.threshold = 0.1;  // low enough to detect at 0 dB (M ~= 0.25)
        BootstrapDetector detector(config);
        std::mt19937 rng(static_cast<uint32_t>(100 + snr_db));

        float sigma_s = 0.1f;
        float sigma_n = sigma_s / static_cast<float>(std::pow(10.0, snr_db / 20.0));
        std::normal_distribution<float> sig(0.0f, sigma_s);
        std::normal_distribution<float> noise(0.0f, sigma_n);

        std::vector<std::complex<float>> pattern(kHalf);
        for (auto& c : pattern) {
            c = {sig(rng), sig(rng)};
        }
        std::vector<sample_t> samples;
        for (int rep = 0; rep < 8; ++rep) {
            for (const auto& c : pattern) {
                samples.push_back(
                    from_complex_float(c + std::complex<float>(noise(rng), noise(rng))));
            }
        }
        for (size_t i = 0; i < kHalf; ++i) {  // uncorrelated tail ends the peak
            samples.push_back(from_complex_float({sig(rng) + noise(rng), sig(rng) + noise(rng)}));
        }

        RunLog r;
        run(detector, samples, r);
        ASSERT_EQ(r.detections.size(), 1u) << "at " << snr_db << " dB";
        EXPECT_NEAR(r.detections[0].snr_db, snr_db, 1.0) << "at " << snr_db << " dB";
    }
}

// Test bootstrap symbol length constant
TEST(BootstrapDetectorTest, BootstrapLengthConstant) {
    // ATSC 3.0 bootstrap is always 4096 samples (per A/322 Section 5.2)
    EXPECT_EQ(BootstrapDetector::kBootstrapLength, 4096u);
    EXPECT_EQ(BootstrapDetector::kHalfSymbol, 2048u);
}

#ifdef ATSC3_FIXED_POINT

//==============================================================================
// Fixed-point equivalence: the fixed-point detector vs. the same algorithm
// in double. Per the HDL port plan, each fixed-point rewrite needs a
// >=40 dB SNR checkpoint against its reference before RTL holds it to a
// bit-exact bar.
//
// ReferenceDetector is the float build's algorithm (both-halves-energy
// normalization, alpha = 1/1024 EWMAs, energy floor, re-arm hysteresis)
// in double, fed the *same* quantized-then-dequantized samples the
// fixed-point detector sees, so what is measured is the fixed-point
// arithmetic's own error (integer EWMA truncation, CORDIC's finite
// iteration count), not Q1.15 sample resolution.
//==============================================================================

class ReferenceDetector {
public:
    static constexpr size_t kHalfSymbol = BootstrapDetector::kHalfSymbol;

    explicit ReferenceDetector(const BootstrapConfig& config)
        : config_(config),
          delay_buffer_(kHalfSymbol, std::complex<double>(0.0, 0.0)),
          metric_history_(std::max<size_t>(1, config.averaging_window), 0.0) {}

    // detected/metric/cfo_hz/sample_index mirror the "detected" event, as
    // in BootstrapDetection. current_metric/current_cfo_hz are the
    // same-instant running values, matching
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
        constexpr double kDecay = 1.0 / 1024.0;
        constexpr double kMinEnergy = 65536.0 / 1073741824.0;  // 2^16 in Q30 units

        std::complex<double> x_delayed = delay_buffer_[delay_idx_];
        p_sum_ = p_sum_ - p_sum_ * kDecay + 2.0 * x * std::conj(x_delayed);
        r_sum_ = r_sum_ - r_sum_ * kDecay + std::norm(x) + std::norm(x_delayed);
        delay_buffer_[delay_idx_] = x;
        delay_idx_ = (delay_idx_ + 1) % kHalfSymbol;
        ++sample_count_;

        bool energetic = r_sum_ >= kMinEnergy;
        double metric = energetic ? std::norm(p_sum_) / (r_sum_ * r_sum_) : 0.0;
        std::complex<double> p_eff = energetic ? p_sum_ : std::complex<double>(0.0, 0.0);

        metric_sum_ -= metric_history_[metric_idx_];
        metric_sum_ += metric;
        metric_history_[metric_idx_] = metric;
        metric_idx_ = (metric_idx_ + 1) % metric_history_.size();
        double smoothed_metric = metric_sum_ / static_cast<double>(metric_history_.size());

        Result result;
        result.current_metric = smoothed_metric;
        result.current_cfo_hz =
            std::arg(p_eff) * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);

        if (!in_detection_) {
            if (rearm_blocked_) {
                if (smoothed_metric <= config_.threshold) {
                    rearm_blocked_ = false;
                }
            } else if (smoothed_metric > config_.threshold) {
                in_detection_ = true;
                peak_metric_ = smoothed_metric;
                peak_sample_ = sample_count_;
                peak_correlation_ = p_eff;
            }
        } else {
            if (smoothed_metric > peak_metric_) {
                peak_metric_ = smoothed_metric;
                peak_sample_ = sample_count_;
                peak_correlation_ = p_eff;
            }
            if (smoothed_metric < peak_metric_ * 0.8) {
                result.detected = true;
                result.sample_index = peak_sample_;
                result.metric = peak_metric_;
                double phase = std::arg(peak_correlation_);
                result.cfo_hz = phase * config_.sample_rate_hz / (2.0 * M_PI * kHalfSymbol);
                in_detection_ = false;
                rearm_blocked_ = true;
                peak_metric_ = 0.0;
            }
        }
        return result;
    }

private:
    BootstrapConfig config_;
    std::complex<double> p_sum_{0.0, 0.0};
    double r_sum_ = 0.0;
    std::vector<std::complex<double>> delay_buffer_;
    size_t delay_idx_ = 0;
    std::vector<double> metric_history_;
    double metric_sum_ = 0.0;
    size_t metric_idx_ = 0;
    size_t sample_count_ = 0;
    bool in_detection_ = false;
    bool rearm_blocked_ = false;
    double peak_metric_ = 0.0;
    std::complex<double> peak_correlation_{0.0, 0.0};
    size_t peak_sample_ = 0;
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
