// test_null_source.cc — Unit tests for NullSource

#include <gtest/gtest.h>

#include "iq_source.h"
#include "iq_source_factory.h"

#include <complex>
#include <vector>

namespace atsc3 {
namespace hal {
namespace {

TEST(NullSourceTest, CreateSucceeds) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);
    EXPECT_TRUE(source->is_connected());
}

TEST(NullSourceTest, CreateWithSampleRate) {
    auto source = create_null_source(12.5e6);
    ASSERT_NE(source, nullptr);
    EXPECT_DOUBLE_EQ(source->get_sample_rate(), 12.5e6);
}

TEST(NullSourceTest, ReadReturnsZeros) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    std::vector<std::complex<float>> buf(1024);

    // Fill with non-zero values first
    for (auto& s : buf) {
        s = std::complex<float>(1.0f, 1.0f);
    }

    size_t n = source->read(buf.data(), buf.size());
    EXPECT_EQ(n, buf.size());

    // Verify all samples are zero
    for (const auto& s : buf) {
        EXPECT_FLOAT_EQ(s.real(), 0.0f);
        EXPECT_FLOAT_EQ(s.imag(), 0.0f);
    }
}

TEST(NullSourceTest, ReadFillsExactly) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    // Test various buffer sizes
    for (size_t size : {1, 8, 100, 1000, 4096}) {
        std::vector<std::complex<float>> buf(size);
        size_t n = source->read(buf.data(), size);
        EXPECT_EQ(n, size) << "Failed for size " << size;
    }
}

TEST(NullSourceTest, SetFrequencyValid) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    IQSourceError err = source->set_frequency(500e6);
    EXPECT_EQ(err, IQSourceError::kSuccess);
    EXPECT_DOUBLE_EQ(source->get_frequency(), 500e6);
}

TEST(NullSourceTest, SetFrequencyOutOfRange) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    // Too low
    IQSourceError err = source->set_frequency(100.0);
    EXPECT_EQ(err, IQSourceError::kFrequencyOutOfRange);

    // Too high
    err = source->set_frequency(100e9);
    EXPECT_EQ(err, IQSourceError::kFrequencyOutOfRange);
}

TEST(NullSourceTest, SetGainValid) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    IQSourceError err = source->set_gain(30.0);
    EXPECT_EQ(err, IQSourceError::kSuccess);
    EXPECT_DOUBLE_EQ(source->get_gain(), 30.0);
}

TEST(NullSourceTest, SetGainOutOfRange) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    // Negative gain
    IQSourceError err = source->set_gain(-10.0);
    EXPECT_EQ(err, IQSourceError::kGainOutOfRange);

    // Too high
    err = source->set_gain(200.0);
    EXPECT_EQ(err, IQSourceError::kGainOutOfRange);
}

TEST(NullSourceTest, SetSampleRateValid) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    IQSourceError err = source->set_sample_rate(12.5e6);
    EXPECT_EQ(err, IQSourceError::kSuccess);
    EXPECT_DOUBLE_EQ(source->get_sample_rate(), 12.5e6);
}

TEST(NullSourceTest, GetRssiReturnsNoiseFloor) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    // NullSource should return noise floor level
    double rssi = source->get_rssi_dbm();
    EXPECT_LE(rssi, -100.0);  // Should be at or below -100 dBm
}

TEST(NullSourceTest, DeviceInfo) {
    auto source = create_null_source();
    ASSERT_NE(source, nullptr);

    EXPECT_FALSE(source->get_device_name().empty());
    EXPECT_TRUE(source->is_connected());

    // Check reasonable ranges
    EXPECT_LT(source->get_min_frequency(), source->get_max_frequency());
    EXPECT_LT(source->get_min_gain(), source->get_max_gain());
    EXPECT_LT(source->get_min_sample_rate(), source->get_max_sample_rate());
}

TEST(NullSourceTest, ErrorStringConversion) {
    EXPECT_STREQ(iq_source_error_string(IQSourceError::kSuccess), "Success");
    EXPECT_STREQ(iq_source_error_string(IQSourceError::kDeviceNotFound),
                 "Device not found");
    EXPECT_STREQ(iq_source_error_string(IQSourceError::kFrequencyOutOfRange),
                 "Frequency out of range");
}

}  // namespace
}  // namespace hal
}  // namespace atsc3
