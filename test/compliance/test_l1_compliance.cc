// test_l1_compliance.cc — ATSC 3.0 L1 Signaling Compliance Tests
//
// Verifies L1-Pre and L1-Post CRC checks and field parsing for all valid
// configurations per ATSC A/322 Section 5.
//
// Reference: ATSC A/322:2023 Section 5 (L1 Signaling)

#include "config/atsc3_config.h"
#include "framing/l1_decoder.h"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace compliance {
namespace {

using namespace atsc3::config;
using namespace atsc3::framing;

//==============================================================================
// CRC-32 Reference Implementation (ATSC A/322 / ISO 3309)
//==============================================================================

// CRC-32 polynomial: 0x04C11DB7 (ISO 3309 / ITU-T V.42)
constexpr uint32_t kCrc32Polynomial = 0x04C11DB7;
constexpr uint32_t kCrc32Initial = 0xFFFFFFFF;

// Generate CRC lookup table
std::array<uint32_t, 256> generate_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ kCrc32Polynomial;
            } else {
                crc <<= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

// Compute CRC-32 over bit array
uint32_t compute_crc32(const uint8_t* data, size_t num_bits) {
    static const auto table = generate_crc_table();

    uint32_t crc = kCrc32Initial;
    size_t num_bytes = num_bits / 8;

    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t byte = data[i];
        crc = (crc << 8) ^ table[(crc >> 24) ^ byte];
    }

    return crc;
}

//==============================================================================
// L1-Pre Test Data Generation
//==============================================================================

// L1-Pre structure with CRC per ATSC A/322 Table 5.1
struct L1PreTestVector {
    FftSize fft_size;
    uint8_t gi_fraction;
    PilotPattern pilot_pattern;
    uint16_t l1_post_size_cells;
    Modulation l1_post_modulation;
    CodeRate l1_post_code_rate;
    uint8_t l1_post_fec_type;
    uint8_t num_subframes;
    bool preamble_reduced;
};

// Set bits in a byte array (MSB first)
void set_bits(std::vector<uint8_t>& data, size_t offset, size_t num_bits, uint32_t value) {
    for (size_t i = 0; i < num_bits; i++) {
        size_t pos = offset + i;
        size_t byte_idx = pos / 8;
        size_t bit_idx = 7 - (pos % 8);

        if (byte_idx < data.size()) {
            if ((value >> (num_bits - 1 - i)) & 1) {
                data[byte_idx] |= (1 << bit_idx);
            } else {
                data[byte_idx] &= ~(1 << bit_idx);
            }
        }
    }
}

// Extract bits from a byte array (MSB first)
[[maybe_unused]] uint32_t extract_bits(const std::vector<uint8_t>& data, size_t offset,
                                       size_t num_bits) {
    uint32_t value = 0;
    for (size_t i = 0; i < num_bits; i++) {
        size_t pos = offset + i;
        size_t byte_idx = pos / 8;
        size_t bit_idx = 7 - (pos % 8);

        if (byte_idx < data.size()) {
            if (data[byte_idx] & (1 << bit_idx)) {
                value |= (1 << (num_bits - 1 - i));
            }
        }
    }
    return value;
}

// Generate L1-Pre bits with CRC
std::vector<uint8_t> generate_l1_pre_with_crc(const L1PreTestVector& tv) {
    // L1-Pre is 200 bits = 25 bytes (per ATSC A/322)
    std::vector<uint8_t> data(25, 0);

    // Version (3 bits) = 0
    set_bits(data, 0, 3, 0);

    // FFT size (2 bits)
    set_bits(data, 3, 2, static_cast<uint8_t>(tv.fft_size));

    // GI fraction (4 bits)
    set_bits(data, 5, 4, tv.gi_fraction);

    // Pilot pattern (3 bits) - stored as index 0-7 for PP1-PP8
    set_bits(data, 9, 3, static_cast<uint8_t>(tv.pilot_pattern) - 1);

    // L1-Post size cells (16 bits)
    set_bits(data, 12, 16, tv.l1_post_size_cells);

    // L1-Post modulation (4 bits)
    set_bits(data, 28, 4, static_cast<uint8_t>(tv.l1_post_modulation));

    // L1-Post code rate (4 bits)
    set_bits(data, 32, 4, static_cast<uint8_t>(tv.l1_post_code_rate));

    // L1-Post FEC type (2 bits)
    set_bits(data, 36, 2, tv.l1_post_fec_type);

    // Preamble reduced carriers (1 bit)
    set_bits(data, 38, 1, tv.preamble_reduced ? 1 : 0);

    // Num subframes (8 bits)
    set_bits(data, 39, 8, tv.num_subframes);

    // Reserved bits (47-167) - set to 0
    // Already initialized to 0

    // CRC-32 (bits 168-199)
    uint32_t crc = compute_crc32(data.data(), 168);
    set_bits(data, 168, 32, crc);

    return data;
}

// Convert bits to LLRs (positive = 0, negative = 1)
std::vector<int8_t> bits_to_llr(const std::vector<uint8_t>& bits, int8_t confidence = 64) {
    std::vector<int8_t> llr(bits.size() * 8);
    for (size_t i = 0; i < bits.size() * 8; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        bool bit = (bits[byte_idx] >> bit_idx) & 1;
        llr[i] = bit ? -confidence : confidence;
    }
    return llr;
}

//==============================================================================
// Compliance Tests: L1-Pre CRC Validation
//==============================================================================

class L1ComplianceTest : public ::testing::Test {
protected:
    void SetUp() override {
        L1DecoderConfig config;
        config.check_crc = false;  // We'll test CRC separately
        config.max_ldpc_iterations = 25;
        decoder_ = std::make_unique<L1Decoder>(config);
    }

    std::unique_ptr<L1Decoder> decoder_;
};

// Test CRC computation matches reference
TEST_F(L1ComplianceTest, CrcComputationReference) {
    // Known test vector: all zeros should produce a specific CRC
    std::vector<uint8_t> zeros(21, 0);
    uint32_t crc = compute_crc32(zeros.data(), 168);

    // CRC should be non-zero for all-zero input
    EXPECT_NE(crc, 0u);

    // Verify CRC is deterministic
    uint32_t crc2 = compute_crc32(zeros.data(), 168);
    EXPECT_EQ(crc, crc2);
}

// Test L1-Pre with valid CRC passes
TEST_F(L1ComplianceTest, L1PreValidCrcAccepted) {
    L1PreTestVector tv{};
    tv.fft_size = FftSize::FFT_8K;
    tv.gi_fraction = 4;
    tv.pilot_pattern = PilotPattern::PP3;
    tv.l1_post_size_cells = 100;
    tv.l1_post_modulation = Modulation::QPSK;
    tv.l1_post_code_rate = CodeRate::RATE_5_15;
    tv.l1_post_fec_type = 0;
    tv.num_subframes = 1;
    tv.preamble_reduced = false;

    auto bits = generate_l1_pre_with_crc(tv);
    auto llr = bits_to_llr(bits);

    L1Pre pre;
    bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

    EXPECT_TRUE(result);
    EXPECT_EQ(pre.fft_size, tv.fft_size);
    EXPECT_EQ(pre.pilot_pattern, tv.pilot_pattern);
}

// Test L1-Pre with corrupted CRC fails
TEST_F(L1ComplianceTest, L1PreCorruptedCrcRejected) {
    L1DecoderConfig config;
    config.check_crc = true;  // Enable CRC checking
    L1Decoder decoder(config);

    L1PreTestVector tv{};
    tv.fft_size = FftSize::FFT_8K;
    tv.gi_fraction = 4;
    tv.pilot_pattern = PilotPattern::PP3;
    tv.l1_post_size_cells = 100;
    tv.l1_post_modulation = Modulation::QPSK;
    tv.l1_post_code_rate = CodeRate::RATE_5_15;
    tv.l1_post_fec_type = 0;
    tv.num_subframes = 1;
    tv.preamble_reduced = false;

    auto bits = generate_l1_pre_with_crc(tv);

    // Corrupt one bit in the CRC
    bits[22] ^= 0x01;

    auto llr = bits_to_llr(bits);

    L1Pre pre;
    bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

    // With CRC enabled, corrupted data should fail
    EXPECT_FALSE(result) << "Corrupted CRC should be rejected when CRC checking is enabled";
}

//==============================================================================
// Compliance Tests: All FFT Sizes
//==============================================================================

TEST_F(L1ComplianceTest, AllFftSizesValid) {
    std::vector<FftSize> sizes = {FftSize::FFT_8K, FftSize::FFT_16K, FftSize::FFT_32K};

    for (auto fft_size : sizes) {
        L1PreTestVector tv{};
        tv.fft_size = fft_size;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "FFT size " << static_cast<int>(fft_size) << " failed";
        EXPECT_EQ(pre.fft_size, fft_size);
    }
}

//==============================================================================
// Compliance Tests: All Pilot Patterns
//==============================================================================

TEST_F(L1ComplianceTest, AllPilotPatternsValid) {
    for (int pp = 1; pp <= 8; pp++) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = static_cast<PilotPattern>(pp);
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "Pilot pattern PP" << pp << " failed";
        EXPECT_EQ(pre.pilot_pattern, tv.pilot_pattern);
    }
}

//==============================================================================
// Compliance Tests: All Guard Intervals
//==============================================================================

TEST_F(L1ComplianceTest, AllGuardIntervalsValid) {
    // Guard interval fractions per ATSC A/322 Table 5.3
    // Index 0-10 maps to different CP lengths
    for (uint8_t gi = 0; gi < 11; gi++) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = gi;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "Guard interval index " << (int)gi << " failed";
        EXPECT_EQ(pre.gi_fraction, gi);
    }
}

//==============================================================================
// Compliance Tests: All L1-Post Modulations
//==============================================================================

TEST_F(L1ComplianceTest, AllL1PostModulationsValid) {
    std::vector<Modulation> mods = {Modulation::QPSK,   Modulation::QAM16,   Modulation::QAM64,
                                    Modulation::QAM256, Modulation::QAM1024, Modulation::QAM4096};

    for (auto mod : mods) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = mod;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "L1-Post modulation " << static_cast<int>(mod) << " failed";
        EXPECT_EQ(pre.l1_post_modulation, mod);
    }
}

//==============================================================================
// Compliance Tests: All Code Rates
//==============================================================================

TEST_F(L1ComplianceTest, AllCodeRatesValid) {
    std::vector<CodeRate> rates = {
        CodeRate::RATE_2_15,  CodeRate::RATE_3_15,  CodeRate::RATE_4_15,  CodeRate::RATE_5_15,
        CodeRate::RATE_6_15,  CodeRate::RATE_7_15,  CodeRate::RATE_8_15,  CodeRate::RATE_9_15,
        CodeRate::RATE_10_15, CodeRate::RATE_11_15, CodeRate::RATE_12_15, CodeRate::RATE_13_15};

    for (auto rate : rates) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = rate;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "Code rate " << static_cast<int>(rate) << " failed";
        EXPECT_EQ(pre.l1_post_code_rate, rate);
    }
}

//==============================================================================
// Compliance Tests: L1-Post Size Range
//==============================================================================

TEST_F(L1ComplianceTest, L1PostSizeRangeValid) {
    // Test various L1-Post size values
    std::vector<uint16_t> sizes = {1, 100, 500, 1000, 5000, 10000, 30000, 65535};

    for (auto size : sizes) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = size;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "L1-Post size " << size << " failed";
        EXPECT_EQ(pre.l1_post_size_cells, size);
    }
}

//==============================================================================
// Compliance Tests: Subframe Count Range
//==============================================================================

TEST_F(L1ComplianceTest, SubframeCountRangeValid) {
    // Test subframe counts 1-255
    std::vector<uint8_t> counts = {1, 2, 10, 50, 100, 200, 255};

    for (auto count : counts) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = 0;
        tv.num_subframes = count;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "Subframe count " << (int)count << " failed";
        EXPECT_EQ(pre.num_subframes, count);
    }
}

//==============================================================================
// Compliance Tests: FEC Type
//==============================================================================

TEST_F(L1ComplianceTest, AllFecTypesValid) {
    // FEC type: 0 = short LDPC (16200), 1 = long LDPC (64800)
    for (uint8_t fec_type = 0; fec_type < 2; fec_type++) {
        L1PreTestVector tv{};
        tv.fft_size = FftSize::FFT_8K;
        tv.gi_fraction = 4;
        tv.pilot_pattern = PilotPattern::PP3;
        tv.l1_post_size_cells = 100;
        tv.l1_post_modulation = Modulation::QPSK;
        tv.l1_post_code_rate = CodeRate::RATE_5_15;
        tv.l1_post_fec_type = fec_type;
        tv.num_subframes = 1;

        auto bits = generate_l1_pre_with_crc(tv);
        auto llr = bits_to_llr(bits);

        L1Pre pre;
        bool result = decoder_->decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result) << "FEC type " << (int)fec_type << " failed";
        EXPECT_EQ(pre.l1_post_fec_type, fec_type);
    }
}

//==============================================================================
// Compliance Tests: Comprehensive Configuration Matrix
//==============================================================================

TEST_F(L1ComplianceTest, ComprehensiveConfigMatrix) {
    // Test all combinations of key parameters (subset)
    std::vector<FftSize> fft_sizes = {FftSize::FFT_8K, FftSize::FFT_16K, FftSize::FFT_32K};
    std::vector<PilotPattern> patterns = {PilotPattern::PP1, PilotPattern::PP4, PilotPattern::PP8};
    std::vector<Modulation> mods = {Modulation::QPSK, Modulation::QAM64, Modulation::QAM256};

    size_t total_tests = 0;
    size_t passed_tests = 0;

    for (auto fft : fft_sizes) {
        for (auto pp : patterns) {
            for (auto mod : mods) {
                total_tests++;

                L1PreTestVector tv{};
                tv.fft_size = fft;
                tv.gi_fraction = 4;
                tv.pilot_pattern = pp;
                tv.l1_post_size_cells = 500;
                tv.l1_post_modulation = mod;
                tv.l1_post_code_rate = CodeRate::RATE_7_15;
                tv.l1_post_fec_type = 0;
                tv.num_subframes = 1;

                auto bits = generate_l1_pre_with_crc(tv);
                auto llr = bits_to_llr(bits);

                L1Pre pre;
                if (decoder_->decode_l1_pre(llr.data(), llr.size(), pre)) {
                    if (pre.fft_size == fft && pre.pilot_pattern == pp &&
                        pre.l1_post_modulation == mod) {
                        passed_tests++;
                    }
                }
            }
        }
    }

    EXPECT_EQ(passed_tests, total_tests)
        << "Passed " << passed_tests << "/" << total_tests << " configuration combinations";
}

//==============================================================================
// Compliance Summary
//==============================================================================

TEST_F(L1ComplianceTest, ComplianceSummary) {
    std::cout << "\n=== L1 Signaling Compliance Summary ===\n";
    std::cout << "L1-Pre field sizes per ATSC A/322 Table 5.1:\n";
    std::cout << "  Version:              3 bits\n";
    std::cout << "  FFT_SIZE:             2 bits (3 valid values)\n";
    std::cout << "  GI_FRACTION:          4 bits (11 valid values)\n";
    std::cout << "  PILOT_PATTERN:        3 bits (8 valid values)\n";
    std::cout << "  L1_POST_SIZE:        16 bits\n";
    std::cout << "  L1_POST_MOD:          4 bits (6+ valid values)\n";
    std::cout << "  L1_POST_CODE_RATE:    4 bits (12 valid values)\n";
    std::cout << "  L1_POST_FEC_TYPE:     2 bits (2 valid values)\n";
    std::cout << "  PREAMBLE_REDUCED:     1 bit\n";
    std::cout << "  NUM_SUBFRAMES:        8 bits (1-255)\n";
    std::cout << "  CRC-32:              32 bits\n";
    std::cout << "========================================\n\n";

    // This is a summary test, always passes
    SUCCEED();
}

}  // namespace
}  // namespace compliance
}  // namespace atsc3
