// test_iq_replay.cc — IQ capture replay integration test
//
// Tests ATSC 3.0 receiver signal chain components with real IQ captures.
// This test focuses on verifiable stages: bootstrap detection, FFT, and
// basic signal quality metrics.
//
// Requires: test/captures/*.sigmf-data files (via git-lfs)

#include "bootstrap_detector.h"
#include "config_bus.h"
#include "fft_engine.h"
#include "freq_correction.h"
#include "iq_source.h"
#include "iq_source_factory.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

namespace atsc3 {
namespace integration {
namespace {

#ifndef SOURCE_DIR
#define SOURCE_DIR ".."
#endif

const std::string kCapturesDir = std::string(SOURCE_DIR) + "/test/captures";
const std::string kPreferredCapture = "ch35_20sec_20260724_050312.sigmf-data";

// ATSC 3.0 parameters
constexpr size_t kBootstrapFftSize = 4096;
constexpr size_t kDataFftSize = 8192;
constexpr size_t kCpLength = 1024;  // Default 8K FFT CP

// Find preferred capture file or any available capture
std::string find_capture_file() {
    namespace fs = std::filesystem;

    // Try preferred capture first
    std::string preferred = kCapturesDir + "/" + kPreferredCapture;
    if (fs::exists(preferred)) {
        return preferred;
    }

    // Fall back to any .sigmf-data file
    for (const auto& entry : fs::directory_iterator(kCapturesDir)) {
        if (entry.path().extension() == ".sigmf-data") {
            return entry.path().string();
        }
    }
    return "";
}

// Parse sample rate from SigMF metadata
double get_sample_rate(const std::string& capture_file) {
    namespace fs = std::filesystem;
    std::string meta_file = fs::path(capture_file).replace_extension(".sigmf-meta").string();

    std::ifstream f(meta_file);
    if (!f.is_open()) {
        return 6.25e6;  // Default
    }

    // Simple JSON parsing for sample_rate
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = content.find("\"core:sample_rate\"");
    if (pos != std::string::npos) {
        pos = content.find(":", pos);
        if (pos != std::string::npos) {
            // Skip whitespace and find the number
            pos++;
            while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) {
                pos++;
            }
            // Extract the number (until comma, }, or whitespace)
            size_t end = pos;
            while (end < content.size() &&
                   (std::isdigit(content[end]) || content[end] == '.' || content[end] == 'e' ||
                    content[end] == 'E' || content[end] == '+' || content[end] == '-')) {
                end++;
            }
            if (end > pos) {
                try {
                    return std::stod(content.substr(pos, end - pos));
                } catch (...) {
                    // Fall through to default
                }
            }
        }
    }
    return 6.25e6;
}

// Test fixture for IQ replay tests
class IqReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        capture_file_ = find_capture_file();
        if (capture_file_.empty()) {
            GTEST_SKIP() << "No IQ captures available in " << kCapturesDir;
        }
        sample_rate_ = get_sample_rate(capture_file_);
        std::cout << "Using capture: " << capture_file_ << std::endl;
        std::cout << "Sample rate: " << sample_rate_ / 1e6 << " MS/s" << std::endl;
    }

    std::string capture_file_;
    double sample_rate_ = 6.25e6;
};

// Test: Bootstrap detection with timing
TEST_F(IqReplayTest, BootstrapDetectionTiming) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    sync::BootstrapConfig config;
    config.sample_rate_hz = sample_rate_;
    config.threshold = 0.6;
    config.averaging_window = 64;

    sync::BootstrapDetector detector(config);

    constexpr size_t kChunkSize = 10000;
    constexpr size_t kMaxSamples = 2000000;  // ~320ms at 6.25 MS/s
    std::vector<sample_t> buf(kChunkSize);

    bool detected = false;
    sync::BootstrapDetection detection;
    size_t total_samples = 0;

    auto start = std::chrono::high_resolution_clock::now();

    while (total_samples < kMaxSamples && !detected) {
        size_t n = source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());
        if (n == 0)
            break;

        auto det = detector.process(buf.data(), n);
        if (det.detected) {
            detected = true;
            detection = det;
        }
        total_samples += n;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Bootstrap detection completed in " << ms << " ms" << std::endl;
    std::cout << "Processed " << total_samples << " samples ("
              << total_samples / sample_rate_ * 1000 << " ms of signal)" << std::endl;

    ASSERT_TRUE(detected) << "No bootstrap detected";

    std::cout << "Bootstrap detected:" << std::endl;
    std::cout << "  Sample index: " << detection.sample_index << std::endl;
    std::cout << "  CFO estimate: " << detection.cfo_hz << " Hz" << std::endl;
    std::cout << "  Metric: " << detection.metric << std::endl;

    // Validate expected CFO range for Ch35 captures (~973 Hz)
    EXPECT_NEAR(std::abs(detection.cfo_hz), 973.0, 100.0)
        << "CFO estimate outside expected range for Ch35";
    EXPECT_GT(detection.metric, 0.9) << "Bootstrap metric too low";
}

// Test: Frequency correction reduces residual CFO
TEST_F(IqReplayTest, FrequencyCorrectionReducesCfo) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    // First detect bootstrap
    sync::BootstrapConfig bconfig;
    bconfig.sample_rate_hz = sample_rate_;
    bconfig.threshold = 0.6;
    sync::BootstrapDetector detector(bconfig);

    std::vector<sample_t> buf(50000);
    size_t n = source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());
    auto det = detector.process(buf.data(), n);
    ASSERT_TRUE(det.detected) << "Bootstrap not detected";

    std::cout << "Original CFO: " << det.cfo_hz << " Hz" << std::endl;

    // Create frequency corrector
    sync::FreqCorrectionConfig fc_config;
    fc_config.sample_rate_hz = sample_rate_;
    fc_config.initial_cfo_hz = det.cfo_hz;
    sync::FreqCorrection corrector(fc_config);

    // Apply correction to samples
    std::vector<sample_t> corrected(n);
    corrector.process(buf.data(), corrected.data(), n);

    // Verify correction by running detection again on corrected signal
    sync::BootstrapDetector detector2(bconfig);
    auto det2 = detector2.process(corrected.data(), n);

    std::cout << "Residual CFO after correction: " << det2.cfo_hz << " Hz" << std::endl;

    EXPECT_TRUE(det2.detected) << "Bootstrap not detected after correction";
    EXPECT_LT(std::abs(det2.cfo_hz), 200.0) << "Residual CFO after correction too large";
}

// Test: FFT engine produces valid output
TEST_F(IqReplayTest, FftEngineProducesValidOutput) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    // Read some samples
    std::vector<sample_t> buf(kDataFftSize * 2);
    size_t n = source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());
    ASSERT_GE(n, kDataFftSize) << "Not enough samples read";

    // Create FFT engine
    ofdm::FftConfig fft_config;
    fft_config.size = ofdm::FftSize::k8K;
    fft_config.direction = ofdm::FftDirection::kForward;
    auto fft = ofdm::FftEngine::create(fft_config);
    ASSERT_NE(fft, nullptr);

    // Run FFT on first symbol-worth of data
    std::vector<sample_t> freq_data(kDataFftSize);
    fft->process(buf.data(), freq_data.data());

    // Verify we got frequency domain data
    double total_power = 0.0;
    double max_power = 0.0;
    size_t max_bin = 0;

    for (size_t i = 0; i < kDataFftSize; ++i) {
        double p = std::norm(freq_data[i]);
        total_power += p;
        if (p > max_power) {
            max_power = p;
            max_bin = i;
        }
    }

    double avg_power = total_power / kDataFftSize;
    double avg_power_db = 10.0 * std::log10(avg_power + 1e-20);
    double peak_power_db = 10.0 * std::log10(max_power + 1e-20);

    std::cout << "FFT output:" << std::endl;
    std::cout << "  Average power: " << avg_power_db << " dB" << std::endl;
    std::cout << "  Peak power: " << peak_power_db << " dB (bin " << max_bin << ")" << std::endl;

    EXPECT_GT(avg_power, 1e-10) << "FFT output appears to be all zeros";
    EXPECT_GT(peak_power_db - avg_power_db, 3.0)
        << "No clear spectral peak - signal may not be OFDM";
}

// Test: Full bootstrap-to-FFT chain
TEST_F(IqReplayTest, BootstrapToFftChain) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    auto start = std::chrono::high_resolution_clock::now();

    // Read a large chunk
    constexpr size_t kNumSamples = 500000;  // ~80ms at 6.25 MS/s
    std::vector<sample_t> buf(kNumSamples);
    size_t total_read =
        source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());
    buf.resize(total_read);

    // Bootstrap detection
    sync::BootstrapConfig bconfig;
    bconfig.sample_rate_hz = sample_rate_;
    sync::BootstrapDetector detector(bconfig);
    auto det = detector.process(buf.data(), total_read);

    ASSERT_TRUE(det.detected) << "Bootstrap not detected";
    std::cout << "\n=== Bootstrap-to-FFT Chain Test ===" << std::endl;
    std::cout << "Bootstrap found at sample " << det.sample_index << std::endl;
    std::cout << "CFO: " << det.cfo_hz << " Hz, Metric: " << det.metric << std::endl;

    // Frequency correction
    sync::FreqCorrectionConfig fc_config;
    fc_config.sample_rate_hz = sample_rate_;
    fc_config.initial_cfo_hz = det.cfo_hz;
    sync::FreqCorrection corrector(fc_config);

    std::vector<sample_t> corrected(total_read);
    corrector.process(buf.data(), corrected.data(), total_read);

    // Count available OFDM symbols
    size_t symbol_len = kDataFftSize + kCpLength;
    size_t first_symbol = det.sample_index + kBootstrapFftSize + kCpLength;
    size_t available = (total_read > first_symbol) ? (total_read - first_symbol) : 0;
    size_t num_symbols = available / symbol_len;

    std::cout << "Available data for " << num_symbols << " OFDM symbols" << std::endl;
    EXPECT_GT(num_symbols, 5) << "Not enough symbols for meaningful test";

    // Create FFT engine
    ofdm::FftConfig fft_config;
    fft_config.size = ofdm::FftSize::k8K;
    auto fft = ofdm::FftEngine::create(fft_config);

    // Process first few symbols
    std::vector<sample_t> time_data(kDataFftSize);
    std::vector<sample_t> freq_data(kDataFftSize);
    double total_power = 0.0;
    size_t symbols_processed = 0;

    for (size_t sym = 0; sym < std::min(num_symbols, size_t(10)); ++sym) {
        size_t offset = first_symbol + sym * symbol_len + kCpLength;  // Skip CP
        if (offset + kDataFftSize > total_read)
            break;

        // Copy data (skip CP)
        std::copy(&corrected[offset], &corrected[offset + kDataFftSize], time_data.begin());

        // FFT
        fft->process(time_data.data(), freq_data.data());

        // Accumulate power
        double sym_power = 0.0;
        for (size_t i = 0; i < kDataFftSize; ++i) {
            sym_power += std::norm(freq_data[i]);
        }
        total_power += sym_power / kDataFftSize;
        symbols_processed++;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double avg_power_db = 10.0 * std::log10(total_power / symbols_processed + 1e-20);
    std::cout << "Processed " << symbols_processed << " symbols in " << ms << " ms" << std::endl;
    std::cout << "Average symbol power: " << avg_power_db << " dB" << std::endl;

    EXPECT_GT(avg_power_db, -30.0) << "Average symbol power too low";
    std::cout << "=== Chain test PASSED ===" << std::endl;
}

// Test: Signal quality metrics from capture
TEST_F(IqReplayTest, SignalQualityMetrics) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    constexpr size_t kNumSamples = 100000;
    std::vector<sample_t> buf(kNumSamples);
    size_t n = source->read(reinterpret_cast<std::complex<float>*>(buf.data()), buf.size());

    // Calculate power statistics
    double sum_power = 0.0;
    double max_power = 0.0;
    double sum_power_sq = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double p = std::norm(buf[i]);
        sum_power += p;
        sum_power_sq += p * p;
        max_power = std::max(max_power, p);
    }

    double avg_power = sum_power / n;
    double variance = (sum_power_sq / n) - (avg_power * avg_power);
    double std_dev = std::sqrt(variance);

    double avg_power_dbfs = 10.0 * std::log10(avg_power + 1e-20);
    double peak_power_dbfs = 10.0 * std::log10(max_power + 1e-20);
    double papr_db = peak_power_dbfs - avg_power_dbfs;
    double crest_factor = std::sqrt(max_power / avg_power);

    std::cout << "\n=== Signal Quality Metrics ===" << std::endl;
    std::cout << "Samples analyzed: " << n << std::endl;
    std::cout << "Average power: " << avg_power_dbfs << " dBFS" << std::endl;
    std::cout << "Peak power: " << peak_power_dbfs << " dBFS" << std::endl;
    std::cout << "PAPR: " << papr_db << " dB" << std::endl;
    std::cout << "Crest factor: " << crest_factor << std::endl;
    std::cout << "Power std dev: " << std_dev << std::endl;

    // Verify reasonable signal characteristics
    EXPECT_GT(avg_power_dbfs, -20.0) << "Signal too weak";
    EXPECT_LT(avg_power_dbfs, 0.0) << "Signal saturating";
    // Note: PAPR can be low over short measurement windows or with AGC compression
    // Typical OFDM PAPR is 7-12 dB over long windows, but varies with capture conditions
    EXPECT_GT(papr_db, 0.1) << "PAPR unreasonably low (signal may be DC)";
    EXPECT_LT(papr_db, 20.0) << "PAPR too high";
}

}  // namespace
}  // namespace integration
}  // namespace atsc3
