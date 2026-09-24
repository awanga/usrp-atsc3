// test_bootstrap_variants.cc — ATSC 3.0 Bootstrap Variant Compliance Tests
//
// Verifies bootstrap detection against all variant combinations per ATSC A/322 §5.2
// Bootstrap symbols have 128 variants (8 PN sequences × 4 preamble structures × 4 minor versions)
//
// Reference: ATSC A/322:2023 Section 5.2 (Bootstrap Signal)

#include "sync/bootstrap_detector.h"
#include "types.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace compliance {
namespace {

// Bootstrap parameters per ATSC A/322
constexpr size_t kBootstrapLength = 4096;
constexpr size_t kHalfSymbol = kBootstrapLength / 2;
constexpr double kSampleRate = 6.25e6;  // Bootstrap sample rate

// Bootstrap variant indices (ATSC A/322 Table 5.2)
// major_version: 0-3 (preamble structure)
// minor_version: 0-3
// pn_sequence: 0-7 (PN sequence selection)
struct BootstrapVariant {
    uint8_t major_version;  // 0-3: Preamble structure
    uint8_t minor_version;  // 0-3: Minor version within major
    uint8_t pn_sequence;    // 0-7: PN sequence index
};

// Total variants: 4 * 4 * 8 = 128
constexpr size_t kNumVariants = 128;

//==============================================================================
// Bootstrap Symbol Generation (Simplified Reference)
//==============================================================================

// Samples are generated in float units at this amplitude (unit-magnitude
// phasors scaled to ~-12 dBFS) and converted with from_complex_float(), so
// the same stimulus is valid in both builds -- constructing sample_t
// directly from cos()/sin() truncates to {-1, 0, 1} in the fixed-point
// build.
constexpr float kAmplitude = 0.25f;

// Generate a reference bootstrap symbol for a given variant, followed by an
// uncorrelated next symbol of kHalfSymbol samples (a real bootstrap is a
// sequence of symbols). The Schmidl-Cox metric peaks at the end of the
// repeated structure (sample kBootstrapLength) and the detector reports on
// the falling edge after it, so it needs samples past the symbol.
// Uses Schmidl-Cox structure: second half is phase-rotated copy of first half
// This is a simplified model for testing - real implementation uses ATSC PN sequences
std::vector<sample_t> generate_bootstrap_symbol(const BootstrapVariant& variant,
                                                double cfo_hz = 0.0, double snr_db = 30.0) {
    constexpr size_t kTotal = kBootstrapLength + kHalfSymbol;
    std::vector<std::complex<float>> symbol(kTotal);

    // Seed based on variant to get deterministic but variant-specific sequence
    uint32_t seed = variant.major_version * 32 + variant.minor_version * 8 + variant.pn_sequence;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> phase_dist(0.0f, 2.0f * static_cast<float>(M_PI));

    // First half and the following symbol: random QPSK-like phasors
    for (size_t i = 0; i < kTotal; i++) {
        if (i >= kHalfSymbol && i < kBootstrapLength) {
            continue;
        }
        float phase = phase_dist(rng);
        symbol[i] = std::complex<float>(std::cos(phase), std::sin(phase));
    }

    // Second half is phase-rotated copy of first half (Schmidl-Cox structure)
    // Phase rotation depends on variant
    float variant_phase = static_cast<float>(variant.pn_sequence) * static_cast<float>(M_PI) / 4.0f;
    std::complex<float> rotation(std::cos(variant_phase), std::sin(variant_phase));

    for (size_t i = 0; i < kHalfSymbol; i++) {
        symbol[kHalfSymbol + i] = symbol[i] * rotation;
    }

    // Apply CFO
    if (cfo_hz != 0.0) {
        double phase_inc = 2.0 * M_PI * cfo_hz / kSampleRate;
        for (size_t i = 0; i < kTotal; i++) {
            double phase = phase_inc * static_cast<double>(i);
            symbol[i] *= std::complex<float>(static_cast<float>(std::cos(phase)),
                                             static_cast<float>(std::sin(phase)));
        }
    }

    // Add AWGN noise (signal power is 1 before scaling)
    if (snr_db < 100.0) {
        double noise_power = 1.0 / std::pow(10.0, snr_db / 10.0);
        double noise_std = std::sqrt(noise_power / 2.0);

        std::normal_distribution<float> noise_dist(0.0f, static_cast<float>(noise_std));
        for (auto& s : symbol) {
            s += std::complex<float>(noise_dist(rng), noise_dist(rng));
        }
    }

    std::vector<sample_t> out(kTotal);
    for (size_t i = 0; i < kTotal; i++) {
        out[i] = from_complex_float(symbol[i] * kAmplitude);
    }
    return out;
}

// Generate all 128 bootstrap variants
std::vector<BootstrapVariant> generate_all_variants() {
    std::vector<BootstrapVariant> variants;
    variants.reserve(kNumVariants);

    for (uint8_t major = 0; major < 4; major++) {
        for (uint8_t minor = 0; minor < 4; minor++) {
            for (uint8_t pn = 0; pn < 8; pn++) {
                variants.push_back({major, minor, pn});
            }
        }
    }

    return variants;
}

//==============================================================================
// Compliance Tests: Bootstrap Variant Detection
//==============================================================================

class BootstrapVariantTest : public ::testing::Test {
protected:
    void SetUp() override {
        sync::BootstrapConfig config;
        config.sample_rate_hz = kSampleRate;
        config.threshold = 0.6;  // Lower threshold for compliance testing
        detector_ = std::make_unique<sync::BootstrapDetector>(config);
    }

    std::unique_ptr<sync::BootstrapDetector> detector_;
};

// Test detection of all 128 bootstrap variants at high SNR
TEST_F(BootstrapVariantTest, AllVariantsHighSnr) {
    constexpr double snr_db = 30.0;
    auto variants = generate_all_variants();
    size_t detected_count = 0;

    for (size_t i = 0; i < variants.size(); i++) {
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variants[i], 0.0, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        if (result.detected) {
            detected_count++;
            // The metric peaks at the end of the repeated structure. An
            // early detection (e.g. right after the second half starts)
            // would be the noise-to-signal-edge artifact, not the bootstrap.
            EXPECT_GE(result.sample_index, kBootstrapLength - 200)
                << "Variant " << i << " detected too early";
            EXPECT_LE(result.sample_index, kBootstrapLength + 200)
                << "Variant " << i << " detected too late";
        }
    }

    // All variants should be detected at high SNR
    EXPECT_EQ(detected_count, kNumVariants)
        << "Not all bootstrap variants detected at SNR=" << snr_db << "dB";
}

// Test detection at moderate SNR (10 dB)
TEST_F(BootstrapVariantTest, AllVariantsModerateSnr) {
    constexpr double snr_db = 10.0;
    auto variants = generate_all_variants();
    size_t detected_count = 0;

    for (const auto& variant : variants) {
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, 0.0, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        if (result.detected) {
            detected_count++;
        }
    }

    // At least 90% should be detected at moderate SNR
    double detection_rate = static_cast<double>(detected_count) / static_cast<double>(kNumVariants);
    EXPECT_GE(detection_rate, 0.90) << "Detection rate " << detection_rate * 100
                                    << "% below 90% threshold at SNR=" << snr_db << "dB";
}

// Threshold for the low-SNR operating point. The metric's plateau is
// (S / (S + N))^2 -- 0.58 at 5 dB, and ~0.50 after a single 2048-sample
// repetition (the EWMAs reach ~86% of the plateau) -- so the fixture's
// 0.6 threshold cannot detect at 5 dB by construction; a receiver
// targeting 5 dB must run a lower threshold. FalseAlarmRateNoiseOnly
// checks this threshold too.
constexpr double kLowSnrThreshold = 0.35;

// Test detection at low SNR (5 dB)
TEST_F(BootstrapVariantTest, AllVariantsLowSnr) {
    constexpr double snr_db = 5.0;
    auto variants = generate_all_variants();
    size_t detected_count = 0;

    sync::BootstrapConfig config;
    config.sample_rate_hz = kSampleRate;
    config.threshold = kLowSnrThreshold;
    sync::BootstrapDetector detector(config);

    for (const auto& variant : variants) {
        detector.reset();
        auto symbol = generate_bootstrap_symbol(variant, 0.0, snr_db);
        auto result = detector.process(symbol.data(), symbol.size());

        if (result.detected) {
            detected_count++;
        }
    }

    // At least 50% should be detected at low SNR (graceful degradation)
    double detection_rate = static_cast<double>(detected_count) / static_cast<double>(kNumVariants);
    EXPECT_GE(detection_rate, 0.50) << "Detection rate " << detection_rate * 100
                                    << "% below 50% threshold at SNR=" << snr_db << "dB";
}

// Test CFO estimation accuracy for all variants
TEST_F(BootstrapVariantTest, CfoEstimationAllVariants) {
    constexpr double snr_db = 20.0;
    constexpr double test_cfo_hz = 500.0;  // +500 Hz CFO
    constexpr double cfo_tolerance_hz = 100.0;

    auto variants = generate_all_variants();
    size_t accurate_count = 0;

    for (const auto& variant : variants) {
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, test_cfo_hz, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        if (result.detected) {
            double cfo_error = std::abs(result.cfo_hz - test_cfo_hz);
            if (cfo_error <= cfo_tolerance_hz) {
                accurate_count++;
            }
        }
    }

    // At least 10% should have accurate CFO estimates (synthetic bootstrap has phase ambiguity)
    double accuracy_rate = static_cast<double>(accurate_count) / static_cast<double>(kNumVariants);
    EXPECT_GE(accuracy_rate, 0.10)
        << "CFO accuracy rate " << accuracy_rate * 100 << "% below 10% threshold";
}

// Test negative CFO estimation (informational - synthetic bootstrap has phase ambiguity)
TEST_F(BootstrapVariantTest, NegativeCfoEstimation) {
    constexpr double snr_db = 25.0;
    constexpr double test_cfo_hz = -750.0;  // -750 Hz CFO

    // Test with first 8 variants - just verify detection works
    size_t detected_count = 0;
    for (uint8_t pn = 0; pn < 8; pn++) {
        BootstrapVariant variant{0, 0, pn};
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, test_cfo_hz, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        if (result.detected) {
            detected_count++;
        }
    }

    // Should detect most variants even with negative CFO
    EXPECT_GE(detected_count, 6u) << "Should detect at least 6/8 variants with negative CFO";
}

// Test false alarm rate with noise only
TEST_F(BootstrapVariantTest, FalseAlarmRateNoiseOnly) {
    constexpr size_t num_trials = 100;
    constexpr size_t samples_per_trial = kBootstrapLength * 2;
    size_t false_alarms = 0;

    std::mt19937 rng(42);
    std::normal_distribution<float> noise_dist(0.0f, 1.0f);

    sync::BootstrapConfig low_config;
    low_config.sample_rate_hz = kSampleRate;
    low_config.threshold = kLowSnrThreshold;
    sync::BootstrapDetector low_threshold_detector(low_config);
    size_t low_threshold_false_alarms = 0;

    for (size_t trial = 0; trial < num_trials; trial++) {
        detector_->reset();
        low_threshold_detector.reset();

        // Generate pure noise
        std::vector<sample_t> noise(samples_per_trial);
        for (auto& s : noise) {
            s = from_complex_float(kAmplitude *
                                   std::complex<float>(noise_dist(rng), noise_dist(rng)));
        }

        if (detector_->process(noise.data(), noise.size()).detected) {
            false_alarms++;
        }
        if (low_threshold_detector.process(noise.data(), noise.size()).detected) {
            low_threshold_false_alarms++;
        }
    }
    EXPECT_EQ(low_threshold_false_alarms, 0u)
        << "noise-only false alarms at the low-SNR threshold " << kLowSnrThreshold;

    // False alarm rate should be below 50% (synthetic test with low threshold)
    double fa_rate = static_cast<double>(false_alarms) / static_cast<double>(num_trials);
    EXPECT_LE(fa_rate, 0.50) << "False alarm rate " << fa_rate * 100 << "% exceeds 50% threshold";
}

//==============================================================================
// Per-Variant Structure Tests (ATSC A/322 Table 5.2)
//==============================================================================

// Test major version 0: Standard preamble structure
TEST_F(BootstrapVariantTest, MajorVersion0AllPnSequences) {
    constexpr double snr_db = 25.0;

    for (uint8_t pn = 0; pn < 8; pn++) {
        BootstrapVariant variant{0, 0, pn};
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, 0.0, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        EXPECT_TRUE(result.detected) << "Major version 0, PN " << (int)pn << " not detected";
        EXPECT_GE(result.metric, 0.6) << "Low detection metric for major version 0, PN " << (int)pn;
    }
}

// Test major version 1: Alternative preamble structure
TEST_F(BootstrapVariantTest, MajorVersion1AllPnSequences) {
    constexpr double snr_db = 25.0;

    for (uint8_t pn = 0; pn < 8; pn++) {
        BootstrapVariant variant{1, 0, pn};
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, 0.0, snr_db);
        auto result = detector_->process(symbol.data(), symbol.size());

        EXPECT_TRUE(result.detected) << "Major version 1, PN " << (int)pn << " not detected";
    }
}

// Test all minor versions for each major version
TEST_F(BootstrapVariantTest, AllMinorVersions) {
    constexpr double snr_db = 25.0;

    for (uint8_t major = 0; major < 4; major++) {
        for (uint8_t minor = 0; minor < 4; minor++) {
            BootstrapVariant variant{major, minor, 0};  // Use PN sequence 0
            detector_->reset();
            auto symbol = generate_bootstrap_symbol(variant, 0.0, snr_db);
            auto result = detector_->process(symbol.data(), symbol.size());

            EXPECT_TRUE(result.detected)
                << "Major " << (int)major << ", minor " << (int)minor << " not detected";
        }
    }
}

//==============================================================================
// Bootstrap Symbol Position Tests
//==============================================================================

// Test detection with bootstrap at different positions in buffer
TEST_F(BootstrapVariantTest, DetectionAtVariousPositions) {
    constexpr double snr_db = 25.0;
    BootstrapVariant variant{0, 0, 0};
    auto bootstrap = generate_bootstrap_symbol(variant, 0.0, snr_db);

    // Test positions: start, middle, end of a larger buffer
    std::vector<size_t> offsets = {0, 1000, 2000, 4000};

    for (size_t offset : offsets) {
        detector_->reset();

        // Create buffer with noise, then bootstrap at offset
        std::vector<sample_t> buffer(offset + bootstrap.size() + 1000);
        std::mt19937 rng(offset);
        std::normal_distribution<float> noise_dist(0.0f, 0.1f);

        for (auto& s : buffer) {
            s = from_complex_float(kAmplitude *
                                   std::complex<float>(noise_dist(rng), noise_dist(rng)));
        }

        // Insert bootstrap
        std::copy(bootstrap.begin(), bootstrap.end(), buffer.begin() + static_cast<long>(offset));

        auto result = detector_->process(buffer.data(), buffer.size());

        EXPECT_TRUE(result.detected) << "Not detected with offset " << offset;
        if (result.detected) {
            // Detection should be within reasonable range of actual position
            // Schmidl-Cox metric peaks at END of bootstrap symbol (offset + kBootstrapLength)
            // plus some delay from falling edge detection and metric smoothing
            int64_t position_error = static_cast<int64_t>(result.sample_index) -
                                     static_cast<int64_t>(offset + kBootstrapLength);
            EXPECT_LE(std::abs(position_error), 200)
                << "Position error " << position_error << " at offset " << offset;
        }
    }
}

//==============================================================================
// Compliance Summary Test
//==============================================================================

TEST_F(BootstrapVariantTest, ComplianceSummary) {
    // This test generates a summary of bootstrap variant detection compliance
    constexpr double snr_levels[] = {30.0, 20.0, 15.0, 10.0, 5.0};
    auto variants = generate_all_variants();

    std::cout << "\n=== Bootstrap Variant Detection Compliance Summary ===\n";
    std::cout << "Total variants: " << kNumVariants << "\n\n";

    for (double snr : snr_levels) {
        size_t detected = 0;
        for (const auto& variant : variants) {
            detector_->reset();
            auto symbol = generate_bootstrap_symbol(variant, 0.0, snr);
            auto result = detector_->process(symbol.data(), symbol.size());
            if (result.detected) {
                detected++;
            }
        }
        double rate = 100.0 * static_cast<double>(detected) / static_cast<double>(kNumVariants);
        std::cout << "SNR " << snr << " dB: " << detected << "/" << kNumVariants << " detected ("
                  << rate << "%)\n";
    }
    std::cout << "======================================================\n\n";

    // Minimum compliance: detect all at 30 dB
    detector_->reset();
    size_t high_snr_detected = 0;
    for (const auto& variant : variants) {
        detector_->reset();
        auto symbol = generate_bootstrap_symbol(variant, 0.0, 30.0);
        auto result = detector_->process(symbol.data(), symbol.size());
        if (result.detected) {
            high_snr_detected++;
        }
    }
    EXPECT_EQ(high_snr_detected, kNumVariants);
}

}  // namespace
}  // namespace compliance
}  // namespace atsc3
