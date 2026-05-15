// test_file_source.cc — Unit tests for FileSource

#include "iq_source.h"
#include "iq_source_factory.h"

#include <complex>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace atsc3 {
namespace hal {
namespace {

// Helper class to manage temporary IQ files for testing
class TempIQFile {
public:
    TempIQFile(const std::vector<std::complex<float>>& samples) {
        // Create temporary file
        path_ = std::tmpnam(nullptr);
        path_ += ".iq";

        // Write samples
        std::ofstream file(path_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(samples.data()),
                   samples.size() * sizeof(std::complex<float>));
        file.close();
    }

    ~TempIQFile() {
        // Clean up
        std::remove(path_.c_str());
    }

    const std::string& path() const {
        return path_;
    }

private:
    std::string path_;
};

TEST(FileSourceTest, CreateWithValidFile) {
    // Create a test file with 8 samples
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f},   {0.0f, 1.0f}, {-1.0f, 0.0f},
                                                     {0.0f, -1.0f},  {0.5f, 0.5f}, {-0.5f, 0.5f},
                                                     {-0.5f, -0.5f}, {0.5f, -0.5f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6);
    ASSERT_NE(source, nullptr);
    EXPECT_TRUE(source->is_connected());
}

TEST(FileSourceTest, CreateWithInvalidFile) {
    auto source = create_file_source("/nonexistent/path/file.iq", 6.25e6);
    EXPECT_EQ(source, nullptr);
}

TEST(FileSourceTest, ReadBitExact) {
    // Create a test file with known samples
    std::vector<std::complex<float>> test_samples = {{1.0f, 2.0f},   {3.0f, 4.0f},   {5.0f, 6.0f},
                                                     {7.0f, 8.0f},   {-1.0f, -2.0f}, {-3.0f, -4.0f},
                                                     {-5.0f, -6.0f}, {-7.0f, -8.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6);
    ASSERT_NE(source, nullptr);

    std::vector<std::complex<float>> buf(8);
    size_t n = source->read(buf.data(), 8);
    EXPECT_EQ(n, 8u);

    // Verify bit-exact match
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(buf[i].real(), test_samples[i].real())
            << "Mismatch at sample " << i << " real";
        EXPECT_FLOAT_EQ(buf[i].imag(), test_samples[i].imag())
            << "Mismatch at sample " << i << " imag";
    }
}

TEST(FileSourceTest, ReadPartialBuffer) {
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f},
                                                     {2.0f, 0.0f},
                                                     {3.0f, 0.0f},
                                                     {4.0f, 0.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6);
    ASSERT_NE(source, nullptr);

    // Read 2 samples at a time
    std::vector<std::complex<float>> buf(2);

    size_t n = source->read(buf.data(), 2);
    EXPECT_EQ(n, 2u);
    EXPECT_FLOAT_EQ(buf[0].real(), 1.0f);
    EXPECT_FLOAT_EQ(buf[1].real(), 2.0f);

    n = source->read(buf.data(), 2);
    EXPECT_EQ(n, 2u);
    EXPECT_FLOAT_EQ(buf[0].real(), 3.0f);
    EXPECT_FLOAT_EQ(buf[1].real(), 4.0f);
}

TEST(FileSourceTest, ReadPastEOF) {
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f}, {2.0f, 0.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6, false);  // No loop
    ASSERT_NE(source, nullptr);

    std::vector<std::complex<float>> buf(4);
    size_t n = source->read(buf.data(), 4);

    // Should only get 2 samples
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(source->get_last_error(), IQSourceError::kEndOfFile);
}

TEST(FileSourceTest, LoopMode) {
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f}, {2.0f, 0.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6, true);  // Loop enabled
    ASSERT_NE(source, nullptr);

    std::vector<std::complex<float>> buf(4);
    size_t n = source->read(buf.data(), 4);

    // Should get 4 samples (looped)
    EXPECT_EQ(n, 4u);
    EXPECT_FLOAT_EQ(buf[0].real(), 1.0f);
    EXPECT_FLOAT_EQ(buf[1].real(), 2.0f);
    EXPECT_FLOAT_EQ(buf[2].real(), 1.0f);  // Looped
    EXPECT_FLOAT_EQ(buf[3].real(), 2.0f);
}

TEST(FileSourceTest, RssiFromPower) {
    // Create samples with known power
    // Unit amplitude = 0 dBFS, which maps to some reference dBm
    std::vector<std::complex<float>> test_samples;
    for (int i = 0; i < 1000; ++i) {
        test_samples.push_back({0.5f, 0.0f});  // |x|^2 = 0.25 = -6 dBFS
    }
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6);
    ASSERT_NE(source, nullptr);

    std::vector<std::complex<float>> buf(1000);
    source->read(buf.data(), 1000);

    double rssi = source->get_rssi_dbm();
    // Should be around +10 dBm (full scale) - 6 dB = +4 dBm
    // Allow some tolerance for implementation differences
    EXPECT_GT(rssi, -20.0);  // Should be reasonably strong
    EXPECT_LT(rssi, 20.0);   // But not unreasonable
}

TEST(FileSourceTest, SampleRateReported) {
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 12.5e6);
    ASSERT_NE(source, nullptr);

    EXPECT_DOUBLE_EQ(source->get_sample_rate(), 12.5e6);
}

TEST(FileSourceTest, DeviceName) {
    std::vector<std::complex<float>> test_samples = {{1.0f, 0.0f}};
    TempIQFile temp_file(test_samples);

    auto source = create_file_source(temp_file.path(), 6.25e6);
    ASSERT_NE(source, nullptr);

    std::string name = source->get_device_name();
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name.find("FileSource"), std::string::npos);
}

}  // namespace
}  // namespace hal
}  // namespace atsc3
