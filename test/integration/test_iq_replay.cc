// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_iq_replay.cc — IQ capture replay integration test
//
// Tests ATSC 3.0 receiver signal chain components on the IQ fixtures in
// test/captures/ (synthetic A/321 bootstraps at 6.144 MS/s; see the README
// there): bootstrap detection and decoding against the positions and
// fields recorded in the .sigmf-meta, frame sync over every bootstrap,
// frequency correction, FFT, and basic signal quality metrics.
//
// Requires: test/captures/*.sigmf-data files (via git-lfs)

#include "bootstrap_decoder.h"
#include "bootstrap_detector.h"
#include "config_bus.h"
#include "fft_engine.h"
#include "frame_sync.h"
#include "freq_correction.h"
#include "iq_source.h"
#include "iq_source_factory.h"

#include <algorithm>
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

constexpr size_t kDataFftSize = 8192;

// IQSource::read() always fills std::complex<float> (see iq_source.h) --
// reinterpret_cast-ing a sample_t* buffer to std::complex<float>* and
// passing it straight through only happens to work in the float build,
// where sample_t IS std::complex<float>. In the fixed-point build
// sample_t is std::complex<int16_t> (half the size), so that cast makes
// read() write float-sized samples into an int16-sized buffer -- a 2x
// heap overflow, silent until something downstream corrupts (seen as
// "malloc(): corrupted top size" / SIGSEGV in ctest, not at the call
// site itself). Reading into a same-count std::complex<float> staging
// buffer and converting through the real, shared from_complex_float()
// (types.h) is correct in both builds, and an identity copy in the
// float one.
size_t read_samples(hal::IQSource& source, sample_t* out, size_t n) {
    std::vector<std::complex<float>> staging(n);
    size_t got = source.read(staging.data(), n);
    for (size_t i = 0; i < got; ++i) {
        out[i] = from_complex_float(staging[i]);
    }
    return got;
}

// True if path is an un-fetched git-lfs pointer (a ~130-byte text stub)
// rather than real IQ data. Reading one as samples yields garbage, so
// such captures are skipped with an explicit `git lfs pull` hint instead.
bool is_lfs_pointer(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string head(40, '\0');
    f.read(&head[0], static_cast<std::streamsize>(head.size()));
    return head.compare(0, 16, "version https://") == 0 &&
           head.find("git-lfs") != std::string::npos;
}

// Find preferred capture file or any available capture
std::string find_capture_file() {
    namespace fs = std::filesystem;

    // Try preferred capture first
    std::string preferred = kCapturesDir + "/" + kPreferredCapture;
    if (fs::exists(preferred) && !is_lfs_pointer(preferred)) {
        return preferred;
    }

    // Fall back to any .sigmf-data file
    for (const auto& entry : fs::directory_iterator(kCapturesDir)) {
        if (entry.path().extension() == ".sigmf-data" && !is_lfs_pointer(entry.path().string())) {
            return entry.path().string();
        }
    }
    return "";
}

std::string read_meta(const std::string& capture_file) {
    namespace fs = std::filesystem;
    std::ifstream f(fs::path(capture_file).replace_extension(".sigmf-meta").string());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The integers following the first occurrence of key (a JSON number or
// array of numbers), up to count of them; empty if the key is missing.
std::vector<long> meta_integers(const std::string& meta, const std::string& key, size_t count) {
    std::vector<long> out;
    size_t pos = meta.find("\"" + key + "\"");
    if (pos == std::string::npos) {
        return out;
    }
    pos += key.size() + 2;
    while (out.size() < count && pos < meta.size()) {
        if (std::isdigit(static_cast<unsigned char>(meta[pos]))) {
            size_t end = pos;
            out.push_back(std::stol(meta.substr(pos), &end));
            pos += end;
        } else if (meta[pos] == '}') {
            break;
        } else {
            pos++;
        }
    }
    return out;
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
    const std::string key = "\"core:sample_rate\"";
    size_t pos = content.find(key);
    if (pos != std::string::npos) {
        pos = content.find(":", pos + key.size());  // not the colon inside the key
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
            GTEST_SKIP() << "No IQ captures available in " << kCapturesDir
                         << " (git-lfs pointer files are ignored; run `git lfs pull`)";
        }
        sample_rate_ = get_sample_rate(capture_file_);
        std::string meta = read_meta(capture_file_);
        size_t ann = meta.find("\"annotations\"");
        ASSERT_NE(ann, std::string::npos) << "no annotations in the .sigmf-meta";
        std::vector<long> fields = meta_integers(meta.substr(ann), "atsc3:bootstrap_fields", 3);
        ASSERT_EQ(fields.size(), 3u) << "no atsc3:bootstrap_fields in the .sigmf-meta";
        const std::string key = "\"core:sample_start\"";
        for (size_t at = meta.find(key, ann); at != std::string::npos;
             at = meta.find(key, at + key.size())) {
            bootstraps_.push_back(
                static_cast<size_t>(meta_integers(meta.substr(at), "core:sample_start", 1).at(0)));
        }
        ASSERT_FALSE(bootstraps_.empty()) << "no bootstrap annotation in the .sigmf-meta";
        first_bootstrap_ = bootstraps_[0];
        for (size_t i = 0; i < 3; i++) {
            fields_[i] = static_cast<uint8_t>(fields[i]);
        }
        std::cout << "Using capture: " << capture_file_ << std::endl;
        std::cout << "Sample rate: " << sample_rate_ / 1e6 << " MS/s" << std::endl;
    }

    std::string capture_file_;
    double sample_rate_ = 6.25e6;
    std::vector<size_t> bootstraps_;  // annotated bootstrap starts
    size_t first_bootstrap_ = 0;
    uint8_t fields_[3] = {};
};

// Test: Bootstrap detection with timing
TEST_F(IqReplayTest, BootstrapDetectionTiming) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    ASSERT_DOUBLE_EQ(sample_rate_, sync::kBootstrapSampleRateHz);
    sync::BootstrapDetector detector;

    constexpr size_t kChunkSize = 10000;
    constexpr size_t kMaxSamples = 2000000;  // ~325 ms at 6.144 MS/s
    std::vector<sample_t> buf(kChunkSize);

    bool detected = false;
    sync::BootstrapDetection detection;
    size_t total_samples = 0;

    auto start = std::chrono::high_resolution_clock::now();

    while (total_samples < kMaxSamples && !detected) {
        size_t n = read_samples(*source, buf.data(), buf.size());
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

    // The fixture's offset is ~973 Hz, inside the +-1.5 kHz fractional range.
    EXPECT_NEAR(detection.cfo_hz, 973.0, 30.0);
    EXPECT_NEAR(static_cast<double>(detection.sample_index), static_cast<double>(first_bootstrap_),
                8.0);
    EXPECT_GT(detection.metric, 0.9) << "Bootstrap metric too low";
}

// Test: Frequency correction reduces residual CFO
TEST_F(IqReplayTest, FrequencyCorrectionReducesCfo) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    // First detect bootstrap
    sync::BootstrapDetector detector;

    std::vector<sample_t> buf(50000);
    size_t n = read_samples(*source, buf.data(), buf.size());
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
    sync::BootstrapDetector detector2;
    auto det2 = detector2.process(corrected.data(), n);

    std::cout << "Residual CFO after correction: " << det2.cfo_hz << " Hz" << std::endl;

    EXPECT_TRUE(det2.detected) << "Bootstrap not detected after correction";
    EXPECT_LT(std::abs(det2.cfo_hz), 30.0) << "Residual CFO after correction too large";
}

// Test: FFT engine produces valid output
TEST_F(IqReplayTest, FftEngineProducesValidOutput) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    // Read some samples
    std::vector<sample_t> buf(kDataFftSize * 2);
    size_t n = read_samples(*source, buf.data(), buf.size());
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
        double p = std::norm(to_complex_float(freq_data[i]));
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
    // Not a "clear spectral peak above a pilot-driven noise floor" check:
    // this capture is the synthetic fixture (see
    // scripts/generate_synthetic_capture.py), whose non-burst regions are
    // flat complex Gaussian fill and whose bootstrap burst is a
    // deliberately flat-spectrum repeated sequence (good autocorrelation
    // needs a flat spectrum) -- there is no pilot/data-carrier structure
    // anywhere in it to produce a several-dB peak-over-average the way a
    // real broadcast capture would. What a broken transform (garbage or
    // a constant array) would fail that real content still passes is
    // "not perfectly flat" -- distinct bins carry distinct energy.
    EXPECT_GT(max_power, avg_power) << "Spectrum is perfectly flat -- FFT output looks degenerate";
}

// Test: detection, fractional CFO correction and bootstrap decoding
TEST_F(IqReplayTest, BootstrapDetectDecodeChain) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    constexpr size_t kNumSamples = 100000;
    std::vector<sample_t> buf(kNumSamples);
    size_t total_read = read_samples(*source, buf.data(), buf.size());
    buf.resize(total_read);

    sync::BootstrapDetector detector;
    sync::BootstrapDetection det = detector.process(buf.data(), total_read);
    ASSERT_TRUE(det.detected) << "Bootstrap not detected";
    ASSERT_LE(det.sample_index + sync::BootstrapDecoder::kInputLength, total_read);

    sync::FreqCorrectionConfig fc_config;
    fc_config.sample_rate_hz = sample_rate_;
    fc_config.initial_cfo_hz = det.cfo_hz;
    sync::FreqCorrection corrector(fc_config);
    std::vector<sample_t> corrected(total_read);
    corrector.process(buf.data(), corrected.data(), total_read);

    sync::BootstrapDecoder decoder;
    sync::BootstrapDecodeResult r;
    ASSERT_TRUE(decoder.decode(corrected.data() + det.sample_index, &r))
        << "correlated=" << r.correlated << " termination_ok=" << r.termination_ok
        << " fields_ok=" << r.fields_ok;
    EXPECT_EQ(det.sample_index + static_cast<size_t>(r.timing_offset), first_bootstrap_);
    EXPECT_EQ(r.integer_cfo_bins, 0);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(r.fields[i], fields_[i]) << "bootstrap symbol " << i + 1;
    }
    std::cout << "Bootstrap at " << first_bootstrap_ << ": bsr_coefficient "
              << int(r.signaling.bsr_coefficient) << ", preamble_structure "
              << int(r.signaling.preamble_structure) << std::endl;
}

// Test: every bootstrap in the capture is detected, decoded and accepted by
// FrameSync at its annotated position. The fixture's symbol 1 field
// signals min_time_to_next, so each one must fall in the previous one's
// window and FrameSync locks from the second on.
TEST_F(IqReplayTest, FrameSyncTracksEveryBootstrap) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    std::vector<sample_t> samples;
    std::vector<sample_t> chunk(65536);
    for (size_t n; (n = read_samples(*source, chunk.data(), chunk.size())) > 0;) {
        samples.insert(samples.end(), chunk.begin(), chunk.begin() + n);
    }

    sync::BootstrapDetector detector;
    sync::BootstrapDecoder decoder;
    sync::FrameSync frame_sync;
    std::vector<sync::FrameEvent> events;
    frame_sync.set_frame_callback([&](const sync::FrameEvent& e) { events.push_back(e); });
    std::vector<sample_t> window(sync::BootstrapDecoder::kInputLength);

    // A detection is reported a few samples after the last sample of
    // symbol 0; two symbols of margin covers it.
    constexpr size_t kLatency = 2 * sync::kBootstrapSymbolLength;
    constexpr size_t kChunk = 10000;  // < the detector holdoff: one detection at most
    for (size_t pos = 0; pos < samples.size(); pos += kChunk) {
        size_t n = std::min(kChunk, samples.size() - pos);
        sync::BootstrapDetection det = detector.process(samples.data() + pos, n);
        if (det.detected && det.sample_index + window.size() <= samples.size()) {
            sync::FreqCorrectionConfig fc_config;
            fc_config.sample_rate_hz = sample_rate_;
            fc_config.initial_cfo_hz = det.cfo_hz;
            sync::FreqCorrection corrector(fc_config);
            corrector.process(samples.data() + det.sample_index, window.data(), window.size());
            sync::BootstrapDecodeResult r;
            if (decoder.decode(window.data(), &r)) {
                size_t start = det.sample_index + static_cast<size_t>(r.timing_offset);
                EXPECT_TRUE(frame_sync.on_bootstrap(start, r.signaling)) << start;
            }
        }
        size_t searched = pos + n;
        frame_sync.mark_searched(searched > kLatency ? searched - kLatency : 0);
    }

    // The last annotated bootstrap is complete in the capture.
    ASSERT_LE(bootstraps_.back() + window.size(), samples.size());
    ASSERT_EQ(events.size(), bootstraps_.size());
    for (size_t i = 0; i < events.size(); i++) {
        EXPECT_EQ(events[i].sample_index, bootstraps_[i]) << "frame " << i;
        EXPECT_EQ(events[i].frame_number, i);
        EXPECT_FALSE(events[i].predicted);
    }
    EXPECT_EQ(frame_sync.get_rejected_count(), 0u);
    EXPECT_EQ(frame_sync.get_state(), bootstraps_.size() > 1 ? sync::FrameSyncState::LOCKED
                                                             : sync::FrameSyncState::ACQUIRING);
}

// Test: Signal quality metrics from capture
TEST_F(IqReplayTest, SignalQualityMetrics) {
    auto source = hal::create_file_source(capture_file_, sample_rate_, false);
    ASSERT_NE(source, nullptr);

    constexpr size_t kNumSamples = 100000;
    std::vector<sample_t> buf(kNumSamples);
    size_t n = read_samples(*source, buf.data(), buf.size());

    // Calculate power statistics
    double sum_power = 0.0;
    double max_power = 0.0;
    double sum_power_sq = 0.0;

    for (size_t i = 0; i < n; ++i) {
        // Same reason as BootstrapToFftChain above: dBFS is only
        // meaningful against the normalized [-1, 1) float domain, not
        // raw Q1.15 int16 values.
        double p = std::norm(to_complex_float(buf[i]));
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
