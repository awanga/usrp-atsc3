// test_l1_decoder.cc — Unit tests for lib/framing/l1_decoder.h
//
// Tests L1-Pre/L1-Post decoding and configuration building

#include "l1_decoder.h"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

//==============================================================================
// Test Helpers
//==============================================================================

// Create L1-Pre test data with specified parameters
std::vector<int8_t> create_l1_pre_llr(config::FftSize fft_size, config::PilotPattern pp,
                                      config::Modulation l1_post_mod, uint16_t l1_post_size) {
    // Build L1-Pre bits according to spec
    std::vector<uint8_t> bits(32, 0);  // 256 bits = 32 bytes

    // Helper to set bits
    auto set_bits = [&bits](size_t offset, size_t num_bits, uint32_t value) {
        for (size_t i = 0; i < num_bits; i++) {
            size_t pos = offset + i;
            size_t byte_idx = pos / 8;
            size_t bit_idx = 7 - (pos % 8);
            if ((value >> (num_bits - 1 - i)) & 1) {
                bits[byte_idx] |= (1 << bit_idx);
            } else {
                bits[byte_idx] &= ~(1 << bit_idx);
            }
        }
    };

    // Version (3 bits) = 0
    set_bits(0, 3, 0);

    // FFT size (2 bits)
    set_bits(3, 2, static_cast<uint8_t>(fft_size));

    // GI fraction (4 bits) = 4 (maps to CP_1024_8192)
    set_bits(5, 4, 4);

    // Pilot pattern (3 bits) - stored as index 0-7 for PP1-PP8
    set_bits(9, 3, static_cast<uint8_t>(pp) - 1);

    // L1-Post size (16 bits)
    set_bits(12, 16, l1_post_size);

    // L1-Post modulation (4 bits)
    set_bits(28, 4, static_cast<uint8_t>(l1_post_mod));

    // L1-Post code rate (4 bits) = 5 (RATE_7_15)
    set_bits(32, 4, 5);

    // L1-Post FEC type (2 bits) = 0 (short LDPC)
    set_bits(36, 2, 0);

    // Preamble reduced carriers (1 bit) = 0
    set_bits(38, 1, 0);

    // Num subframes (8 bits) = 1
    set_bits(39, 8, 1);

    // Convert to LLRs (positive = 0, negative = 1)
    std::vector<int8_t> llr(bits.size() * 8);
    for (size_t i = 0; i < bits.size() * 8; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        bool bit = (bits[byte_idx] >> bit_idx) & 1;
        llr[i] = bit ? -64 : 64;  // Strong LLR
    }

    return llr;
}

// Create L1-Post test data with specified PLPs
std::vector<int8_t> create_l1_post_llr(uint8_t num_plps,
                                       const std::vector<config::PlpConfig>& plps) {
    std::vector<uint8_t> bits(256, 0);  // Generous size

    auto set_bits = [&bits](size_t offset, size_t num_bits, uint32_t value) {
        for (size_t i = 0; i < num_bits; i++) {
            size_t pos = offset + i;
            size_t byte_idx = pos / 8;
            size_t bit_idx = 7 - (pos % 8);
            if ((value >> (num_bits - 1 - i)) & 1) {
                bits[byte_idx] |= (1 << bit_idx);
            }
        }
    };

    // Num PLPs (6 bits)
    set_bits(0, 6, num_plps);

    // Num subframes (8 bits) = 1
    set_bits(6, 8, 1);

    // PLP records
    size_t offset = 14;
    for (size_t i = 0; i < num_plps && i < plps.size(); i++) {
        const auto& plp = plps[i];

        // PLP ID (6 bits)
        set_bits(offset, 6, plp.plp_id);
        offset += 6;

        // PLP type (1 bit)
        set_bits(offset, 1, plp.plp_type == config::PlpType::DISPERSED ? 1 : 0);
        offset += 1;

        // Modulation (4 bits)
        set_bits(offset, 4, static_cast<uint8_t>(plp.modulation));
        offset += 4;

        // Code rate (4 bits)
        set_bits(offset, 4, static_cast<uint8_t>(plp.code_rate));
        offset += 4;

        // TI mode (2 bits)
        set_bits(offset, 2, static_cast<uint8_t>(plp.ti_mode));
        offset += 2;

        // FEC blocks (15 bits)
        set_bits(offset, 15, plp.num_fec_blocks);
        offset += 15;

        // Codeword length (1 bit)
        set_bits(offset, 1, plp.codeword_length == config::CodewordLength::LONG ? 1 : 0);
        offset += 1;

        // TI depth (4 bits)
        set_bits(offset, 4, plp.ti_depth);
        offset += 4;

        // TI num blocks (10 bits)
        set_bits(offset, 10, plp.ti_num_blocks);
        offset += 10;

        // Pad to PLP_RECORD_BITS
        size_t used = 6 + 1 + 4 + 4 + 2 + 15 + 1 + 4 + 10;
        if (used < l1_post_fields::PLP_RECORD_BITS) {
            offset += (l1_post_fields::PLP_RECORD_BITS - used);
        }
    }

    // Convert to LLRs
    std::vector<int8_t> llr(bits.size() * 8);
    for (size_t i = 0; i < bits.size() * 8; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        bool bit = (bits[byte_idx] >> bit_idx) & 1;
        llr[i] = bit ? -64 : 64;
    }

    return llr;
}

//==============================================================================
// Construction Tests
//==============================================================================

TEST(L1DecoderTest, DefaultConstruction) {
    L1Decoder decoder;

    EXPECT_TRUE(decoder.get_config().check_crc);
    EXPECT_EQ(decoder.get_config().max_ldpc_iterations, 25u);
}

TEST(L1DecoderTest, CustomConfig) {
    L1DecoderConfig config;
    config.check_crc = false;
    config.max_ldpc_iterations = 50;

    L1Decoder decoder(config);

    EXPECT_FALSE(decoder.get_config().check_crc);
    EXPECT_EQ(decoder.get_config().max_ldpc_iterations, 50u);
}

//==============================================================================
// L1-Pre Decode Tests
//==============================================================================

TEST(L1DecoderTest, DecodeL1PreFft8K) {
    L1DecoderConfig config;
    config.check_crc = false;  // Disable CRC for test data

    L1Decoder decoder(config);

    auto llr = create_l1_pre_llr(config::FftSize::FFT_8K, config::PilotPattern::PP3,
                                 config::Modulation::QPSK, 100);

    L1Pre pre;
    bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

    EXPECT_TRUE(result);
    EXPECT_EQ(pre.fft_size, config::FftSize::FFT_8K);
    EXPECT_EQ(pre.pilot_pattern, config::PilotPattern::PP3);
    EXPECT_EQ(pre.l1_post_modulation, config::Modulation::QPSK);
}

TEST(L1DecoderTest, DecodeL1PreFft16K) {
    L1DecoderConfig config;
    config.check_crc = false;

    L1Decoder decoder(config);

    auto llr = create_l1_pre_llr(config::FftSize::FFT_16K, config::PilotPattern::PP5,
                                 config::Modulation::QAM64, 200);

    L1Pre pre;
    bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

    EXPECT_TRUE(result);
    EXPECT_EQ(pre.fft_size, config::FftSize::FFT_16K);
    EXPECT_EQ(pre.pilot_pattern, config::PilotPattern::PP5);
    EXPECT_EQ(pre.l1_post_modulation, config::Modulation::QAM64);
    EXPECT_EQ(pre.l1_post_size_cells, 200u);
}

TEST(L1DecoderTest, DecodeL1PreFft32K) {
    L1DecoderConfig config;
    config.check_crc = false;

    L1Decoder decoder(config);

    auto llr = create_l1_pre_llr(config::FftSize::FFT_32K, config::PilotPattern::PP8,
                                 config::Modulation::QAM256, 500);

    L1Pre pre;
    bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

    EXPECT_TRUE(result);
    EXPECT_EQ(pre.fft_size, config::FftSize::FFT_32K);
    EXPECT_EQ(pre.pilot_pattern, config::PilotPattern::PP8);
    EXPECT_EQ(pre.l1_post_modulation, config::Modulation::QAM256);
}

TEST(L1DecoderTest, DecodeL1PreNullInput) {
    L1Decoder decoder;

    L1Pre pre;
    bool result = decoder.decode_l1_pre(nullptr, 100, pre);

    EXPECT_FALSE(result);
}

TEST(L1DecoderTest, DecodeL1PreZeroLength) {
    L1Decoder decoder;

    std::vector<int8_t> llr(100, 64);
    L1Pre pre;
    bool result = decoder.decode_l1_pre(llr.data(), 0, pre);

    EXPECT_FALSE(result);
}

//==============================================================================
// L1-Post Decode Tests
//==============================================================================

TEST(L1DecoderTest, DecodeL1PostSinglePlp) {
    L1DecoderConfig config;
    config.check_crc = false;

    L1Decoder decoder(config);

    // Create single PLP configuration
    std::vector<config::PlpConfig> plps(1);
    plps[0].plp_id = 0;
    plps[0].modulation = config::Modulation::QAM64;
    plps[0].code_rate = config::CodeRate::RATE_7_15;
    plps[0].ti_mode = config::TimeInterleaveMode::CTI;
    plps[0].num_fec_blocks = 100;

    auto llr = create_l1_post_llr(1, plps);

    L1Pre pre;
    pre.l1_post_code_rate = config::CodeRate::RATE_5_15;
    pre.l1_post_fec_type = 0;

    L1Post post;
    bool result = decoder.decode_l1_post(llr.data(), llr.size(), pre, post);

    EXPECT_TRUE(result);
    EXPECT_EQ(post.configurable.num_plps, 1u);
    EXPECT_EQ(post.plps[0].plp_id, 0u);
    EXPECT_EQ(post.plps[0].modulation, config::Modulation::QAM64);
    EXPECT_EQ(post.plps[0].code_rate, config::CodeRate::RATE_7_15);
}

TEST(L1DecoderTest, DecodeL1PostMultiplePlps) {
    L1DecoderConfig config;
    config.check_crc = false;

    L1Decoder decoder(config);

    // Create multiple PLPs
    std::vector<config::PlpConfig> plps(3);
    plps[0].plp_id = 0;
    plps[0].modulation = config::Modulation::QPSK;
    plps[0].code_rate = config::CodeRate::RATE_4_15;

    plps[1].plp_id = 1;
    plps[1].modulation = config::Modulation::QAM64;
    plps[1].code_rate = config::CodeRate::RATE_7_15;

    plps[2].plp_id = 2;
    plps[2].modulation = config::Modulation::QAM256;
    plps[2].code_rate = config::CodeRate::RATE_10_15;

    auto llr = create_l1_post_llr(3, plps);

    L1Pre pre;
    pre.l1_post_code_rate = config::CodeRate::RATE_5_15;
    pre.l1_post_fec_type = 0;

    L1Post post;
    bool result = decoder.decode_l1_post(llr.data(), llr.size(), pre, post);

    EXPECT_TRUE(result);
    EXPECT_EQ(post.configurable.num_plps, 3u);

    EXPECT_EQ(post.plps[0].plp_id, 0u);
    EXPECT_EQ(post.plps[0].modulation, config::Modulation::QPSK);

    EXPECT_EQ(post.plps[1].plp_id, 1u);
    EXPECT_EQ(post.plps[1].modulation, config::Modulation::QAM64);

    EXPECT_EQ(post.plps[2].plp_id, 2u);
    EXPECT_EQ(post.plps[2].modulation, config::Modulation::QAM256);
}

TEST(L1DecoderTest, DecodeL1PostNullInput) {
    L1Decoder decoder;

    L1Pre pre;
    L1Post post;
    bool result = decoder.decode_l1_post(nullptr, 100, pre, post);

    EXPECT_FALSE(result);
}

//==============================================================================
// Build Config Tests
//==============================================================================

TEST(L1DecoderTest, BuildConfigFromL1) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    // Create L1-Pre
    auto pre_llr = create_l1_pre_llr(config::FftSize::FFT_16K, config::PilotPattern::PP4,
                                     config::Modulation::QAM64, 150);
    L1Pre pre;
    decoder.decode_l1_pre(pre_llr.data(), pre_llr.size(), pre);

    // Create L1-Post
    std::vector<config::PlpConfig> plps(2);
    plps[0].plp_id = 0;
    plps[0].modulation = config::Modulation::QAM64;
    plps[0].code_rate = config::CodeRate::RATE_7_15;
    plps[0].codeword_length = config::CodewordLength::LONG;

    plps[1].plp_id = 1;
    plps[1].modulation = config::Modulation::QAM256;
    plps[1].code_rate = config::CodeRate::RATE_9_15;
    plps[1].codeword_length = config::CodewordLength::SHORT;

    auto post_llr = create_l1_post_llr(2, plps);
    L1Post post;
    decoder.decode_l1_post(post_llr.data(), post_llr.size(), pre, post);

    // Build config
    auto config = decoder.build_config(pre, post);

    EXPECT_EQ(config.fft_size, config::FftSize::FFT_16K);
    EXPECT_EQ(config.pilot_pattern, config::PilotPattern::PP4);
    EXPECT_EQ(config.l1_post_modulation, config::Modulation::QAM64);
    EXPECT_EQ(config.num_plps, 2u);

    EXPECT_EQ(config.plps[0].modulation, config::Modulation::QAM64);
    EXPECT_EQ(config.plps[0].code_rate, config::CodeRate::RATE_7_15);

    EXPECT_EQ(config.plps[1].modulation, config::Modulation::QAM256);
    EXPECT_EQ(config.plps[1].code_rate, config::CodeRate::RATE_9_15);
}

TEST(L1DecoderTest, BuildConfigDerivedParams) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    // Create minimal L1 data
    auto pre_llr = create_l1_pre_llr(config::FftSize::FFT_8K, config::PilotPattern::PP3,
                                     config::Modulation::QPSK, 100);
    L1Pre pre;
    decoder.decode_l1_pre(pre_llr.data(), pre_llr.size(), pre);

    std::vector<config::PlpConfig> plps(1);
    plps[0].plp_id = 0;
    plps[0].modulation = config::Modulation::QAM64;

    auto post_llr = create_l1_post_llr(1, plps);
    L1Post post;
    decoder.decode_l1_post(post_llr.data(), post_llr.size(), pre, post);

    auto config = decoder.build_config(pre, post);

    // Test derived parameters
    EXPECT_EQ(config.get_fft_samples(), 8192u);
    EXPECT_EQ(config.get_active_carriers(), 6913u);
    EXPECT_GT(config.get_cp_samples(), 0u);
}

//==============================================================================
// Observer Tests
//==============================================================================

TEST(L1DecoderTest, ObserverNotification) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    bool observer_called = false;
    config::FftSize received_fft_size = config::FftSize::FFT_8K;

    decoder.add_observer([&](const config::Atsc3Config& config) {
        observer_called = true;
        received_fft_size = config.fft_size;
    });

    // Create and decode L1
    auto pre_llr = create_l1_pre_llr(config::FftSize::FFT_16K, config::PilotPattern::PP3,
                                     config::Modulation::QPSK, 100);
    L1Pre pre;
    decoder.decode_l1_pre(pre_llr.data(), pre_llr.size(), pre);

    std::vector<config::PlpConfig> plps(1);
    auto post_llr = create_l1_post_llr(1, plps);
    L1Post post;
    decoder.decode_l1_post(post_llr.data(), post_llr.size(), pre, post);

    auto config = decoder.build_config(pre, post);
    decoder.notify_observers(config);

    EXPECT_TRUE(observer_called);
    EXPECT_EQ(received_fft_size, config::FftSize::FFT_16K);
}

TEST(L1DecoderTest, MultipleObservers) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    int call_count = 0;

    decoder.add_observer([&](const config::Atsc3Config&) { call_count++; });
    decoder.add_observer([&](const config::Atsc3Config&) { call_count++; });
    decoder.add_observer([&](const config::Atsc3Config&) { call_count++; });

    config::Atsc3Config config;
    decoder.notify_observers(config);

    EXPECT_EQ(call_count, 3);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(L1DecoderTest, Reset) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    // Decode something
    auto llr = create_l1_pre_llr(config::FftSize::FFT_8K, config::PilotPattern::PP3,
                                 config::Modulation::QPSK, 100);
    L1Pre pre;
    decoder.decode_l1_pre(llr.data(), llr.size(), pre);

    // Reset
    decoder.reset();

    // Should still work after reset
    L1Pre pre2;
    bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre2);
    EXPECT_TRUE(result);
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(L1DecoderTest, AllFftSizes) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    std::vector<config::FftSize> sizes = {config::FftSize::FFT_8K, config::FftSize::FFT_16K,
                                          config::FftSize::FFT_32K};

    for (auto fft_size : sizes) {
        auto llr =
            create_l1_pre_llr(fft_size, config::PilotPattern::PP3, config::Modulation::QPSK, 100);
        L1Pre pre;
        bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result);
        EXPECT_EQ(pre.fft_size, fft_size);
    }
}

TEST(L1DecoderTest, AllPilotPatterns) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    for (int pp = 1; pp <= 8; pp++) {
        auto pattern = static_cast<config::PilotPattern>(pp);
        auto llr =
            create_l1_pre_llr(config::FftSize::FFT_8K, pattern, config::Modulation::QPSK, 100);
        L1Pre pre;
        bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result);
        EXPECT_EQ(pre.pilot_pattern, pattern);
    }
}

TEST(L1DecoderTest, AllModulations) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    std::vector<config::Modulation> mods = {config::Modulation::QPSK, config::Modulation::QAM16,
                                            config::Modulation::QAM64, config::Modulation::QAM256,
                                            config::Modulation::QAM1024};

    for (auto mod : mods) {
        auto llr = create_l1_pre_llr(config::FftSize::FFT_8K, config::PilotPattern::PP3, mod, 100);
        L1Pre pre;
        bool result = decoder.decode_l1_pre(llr.data(), llr.size(), pre);

        EXPECT_TRUE(result);
        EXPECT_EQ(pre.l1_post_modulation, mod);
    }
}

TEST(L1DecoderTest, MaxPlps) {
    L1DecoderConfig cfg;
    cfg.check_crc = false;

    L1Decoder decoder(cfg);

    // Create maximum PLPs (using 10 for test, MAX_PLPS is 64)
    constexpr size_t num_plps = 10;
    std::vector<config::PlpConfig> plps(num_plps);
    for (size_t i = 0; i < num_plps; i++) {
        plps[i].plp_id = static_cast<uint8_t>(i);
        plps[i].modulation = config::Modulation::QAM64;
        plps[i].code_rate = config::CodeRate::RATE_7_15;
    }

    auto llr = create_l1_post_llr(static_cast<uint8_t>(num_plps), plps);

    L1Pre pre;
    pre.l1_post_code_rate = config::CodeRate::RATE_5_15;
    pre.l1_post_fec_type = 0;

    L1Post post;
    bool result = decoder.decode_l1_post(llr.data(), llr.size(), pre, post);

    EXPECT_TRUE(result);
    EXPECT_EQ(post.configurable.num_plps, num_plps);

    for (size_t i = 0; i < num_plps; i++) {
        EXPECT_EQ(post.plps[i].plp_id, i);
    }
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
