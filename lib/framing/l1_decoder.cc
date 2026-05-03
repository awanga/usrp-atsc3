// l1_decoder.cc — ATSC 3.0 L1 preamble decoding
//
// Extracts L1-Pre and L1-Post signaling to configure receiver chain.
//
// Reference: ATSC A/322 Section 5 (L1 Signaling)

#include "l1_decoder.h"

#include <algorithm>
#include <cstring>

namespace atsc3 {
namespace framing {

namespace {

// CRC-32 polynomial (ISO 3309, used in ATSC 3.0)
// x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
constexpr uint32_t CRC32_POLY = 0x04C11DB7;

// CRC-32 lookup table (precomputed)
static uint32_t crc32_table[256];
static bool crc32_table_init = false;

void init_crc32_table() {
    if (crc32_table_init)
        return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ CRC32_POLY;
            } else {
                crc <<= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

// Map FFT size index to enum
config::FftSize index_to_fft_size(uint8_t idx) {
    switch (idx) {
        case 0:
            return config::FftSize::FFT_8K;
        case 1:
            return config::FftSize::FFT_16K;
        case 2:
            return config::FftSize::FFT_32K;
        default:
            return config::FftSize::FFT_8K;
    }
}

// Map pilot pattern index to enum
// Index 0-7 maps to PP1-PP8
config::PilotPattern index_to_pilot_pattern(uint8_t idx) {
    if (idx <= 7) {
        return static_cast<config::PilotPattern>(idx + 1);  // PP1=1, PP8=8
    }
    return config::PilotPattern::PP3;  // Default
}

// Map modulation index to enum
config::Modulation index_to_modulation(uint8_t idx) {
    if (idx <= static_cast<uint8_t>(config::Modulation::NUC_4096)) {
        return static_cast<config::Modulation>(idx);
    }
    return config::Modulation::QPSK;
}

// Map code rate index to enum
config::CodeRate index_to_code_rate(uint8_t idx) {
    if (idx <= static_cast<uint8_t>(config::CodeRate::RATE_13_15)) {
        return static_cast<config::CodeRate>(idx);
    }
    return config::CodeRate::RATE_7_15;
}

// Map time interleave mode index to enum
config::TimeInterleaveMode index_to_ti_mode(uint8_t idx) {
    switch (idx) {
        case 0:
            return config::TimeInterleaveMode::NONE;
        case 1:
            return config::TimeInterleaveMode::CTI;
        case 2:
            return config::TimeInterleaveMode::HTI;
        default:
            return config::TimeInterleaveMode::CTI;
    }
}

// Map CP index to enum
config::CpLength index_to_cp_length(uint8_t idx) {
    if (idx <= static_cast<uint8_t>(config::CpLength::CP_4096_8192)) {
        return static_cast<config::CpLength>(idx);
    }
    return config::CpLength::CP_1024_8192;
}

}  // namespace

L1Decoder::L1Decoder(const L1DecoderConfig& config) : config_(config) {
    init_crc32_table();

    // Pre-allocate buffers
    decoded_bits_.reserve(8192);
    llr_buffer_.reserve(65536);
}

L1Decoder::~L1Decoder() = default;

void L1Decoder::reset() {
    decoded_bits_.clear();
    llr_buffer_.clear();
}

void L1Decoder::add_observer(ConfigObserver observer) {
    observers_.push_back(std::move(observer));
}

void L1Decoder::notify_observers(const config::Atsc3Config& config) {
    for (auto& observer : observers_) {
        observer(config);
    }
}

uint32_t L1Decoder::compute_crc32(const uint8_t* data, size_t num_bits) const {
    uint32_t crc = 0xFFFFFFFF;

    size_t num_bytes = num_bits / 8;
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t byte = data[i];
        crc = (crc << 8) ^ crc32_table[(crc >> 24) ^ byte];
    }

    // Handle remaining bits (if any)
    size_t remaining = num_bits % 8;
    if (remaining > 0) {
        uint8_t byte = data[num_bytes] >> (8 - remaining);
        byte <<= (8 - remaining);
        crc = (crc << 8) ^ crc32_table[(crc >> 24) ^ byte];
    }

    return crc ^ 0xFFFFFFFF;
}

bool L1Decoder::validate_crc(const uint8_t* data, size_t num_bits) const {
    if (!config_.check_crc) {
        return true;  // CRC checking disabled
    }

    if (num_bits < 32) {
        return false;
    }

    // CRC is computed over all bits except the CRC itself
    size_t data_bits = num_bits - 32;
    uint32_t computed = compute_crc32(data, data_bits);

    // Extract transmitted CRC
    uint32_t transmitted = extract_bits(data, data_bits, 32);

    return computed == transmitted;
}

uint32_t L1Decoder::extract_bits(const uint8_t* data, size_t bit_offset, size_t num_bits) const {
    uint32_t result = 0;

    for (size_t i = 0; i < num_bits; i++) {
        size_t pos = bit_offset + i;
        size_t byte_idx = pos / 8;
        size_t bit_idx = 7 - (pos % 8);

        if ((data[byte_idx] >> bit_idx) & 1) {
            result |= (1u << (num_bits - 1 - i));
        }
    }

    return result;
}

bool L1Decoder::fec_decode(const int8_t* llr_in, size_t num_bits, bool short_ldpc,
                           config::CodeRate code_rate, std::vector<uint8_t>& out) {
    // For L1 signaling, we use simplified FEC:
    // 1. Hard decision from LLRs (simplified - real impl uses LDPC+BCH)
    // 2. This is a simplified implementation for testing
    // TODO: Integrate full LDPC+BCH decoding for production

    // Parameters reserved for full FEC implementation
    (void)short_ldpc;
    (void)code_rate;

    size_t num_bytes = (num_bits + 7) / 8;
    out.resize(num_bytes);

    for (size_t i = 0; i < num_bits; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);

        // Hard decision: LLR > 0 means bit 0, LLR < 0 means bit 1
        if (i < num_bits && llr_in[i] < 0) {
            out[byte_idx] |= (1 << bit_idx);
        } else {
            out[byte_idx] &= ~(1 << bit_idx);
        }
    }

    return true;
}

L1Pre L1Decoder::unpack_l1_pre(const uint8_t* bits, size_t num_bits) const {
    L1Pre pre;

    using namespace l1_pre_fields;

    // Extract fields
    pre.version = static_cast<uint8_t>(extract_bits(bits, VERSION_OFFSET, VERSION_BITS));

    uint8_t fft_idx = static_cast<uint8_t>(extract_bits(bits, FFT_SIZE_OFFSET, FFT_SIZE_BITS));
    pre.fft_size = index_to_fft_size(fft_idx);

    pre.gi_fraction =
        static_cast<uint8_t>(extract_bits(bits, GI_FRACTION_OFFSET, GI_FRACTION_BITS));

    // CP length is derived from GI fraction
    pre.cp_length = index_to_cp_length(pre.gi_fraction);

    uint8_t pp_idx =
        static_cast<uint8_t>(extract_bits(bits, PILOT_PATTERN_OFFSET, PILOT_PATTERN_BITS));
    pre.pilot_pattern = index_to_pilot_pattern(pp_idx);

    pre.l1_post_size_cells =
        static_cast<uint16_t>(extract_bits(bits, L1_POST_SIZE_OFFSET, L1_POST_SIZE_BITS));

    uint8_t mod_idx =
        static_cast<uint8_t>(extract_bits(bits, L1_POST_MOD_OFFSET, L1_POST_MOD_BITS));
    pre.l1_post_modulation = index_to_modulation(mod_idx);

    uint8_t rate_idx =
        static_cast<uint8_t>(extract_bits(bits, L1_POST_CODE_RATE_OFFSET, L1_POST_CODE_RATE_BITS));
    pre.l1_post_code_rate = index_to_code_rate(rate_idx);

    pre.l1_post_fec_type =
        static_cast<uint8_t>(extract_bits(bits, L1_POST_FEC_TYPE_OFFSET, L1_POST_FEC_TYPE_BITS));

    pre.preamble_reduced_carriers =
        extract_bits(bits, PREAMBLE_REDUCED_OFFSET, PREAMBLE_REDUCED_BITS) != 0;

    pre.num_subframes =
        static_cast<uint8_t>(extract_bits(bits, NUM_SUBFRAMES_OFFSET, NUM_SUBFRAMES_BITS));

    // Validate CRC
    pre.crc_valid = validate_crc(bits, num_bits);

    return pre;
}

config::PlpConfig L1Decoder::unpack_plp_config(const uint8_t* bits, size_t& bit_offset) const {
    using namespace l1_post_fields;

    config::PlpConfig plp;

    plp.plp_id = static_cast<uint8_t>(extract_bits(bits, bit_offset, PLP_ID_BITS));
    bit_offset += PLP_ID_BITS;

    uint8_t type_idx = static_cast<uint8_t>(extract_bits(bits, bit_offset, PLP_TYPE_BITS));
    plp.plp_type = type_idx == 0 ? config::PlpType::NON_DISPERSED : config::PlpType::DISPERSED;
    bit_offset += PLP_TYPE_BITS;

    uint8_t mod_idx = static_cast<uint8_t>(extract_bits(bits, bit_offset, PLP_MOD_BITS));
    plp.modulation = index_to_modulation(mod_idx);
    bit_offset += PLP_MOD_BITS;

    uint8_t rate_idx = static_cast<uint8_t>(extract_bits(bits, bit_offset, PLP_CODE_RATE_BITS));
    plp.code_rate = index_to_code_rate(rate_idx);
    bit_offset += PLP_CODE_RATE_BITS;

    uint8_t ti_idx = static_cast<uint8_t>(extract_bits(bits, bit_offset, PLP_TI_MODE_BITS));
    plp.ti_mode = index_to_ti_mode(ti_idx);
    bit_offset += PLP_TI_MODE_BITS;

    plp.num_fec_blocks = static_cast<uint16_t>(extract_bits(bits, bit_offset, PLP_FEC_BLOCK_BITS));
    bit_offset += PLP_FEC_BLOCK_BITS;

    // Codeword length (1 bit)
    uint8_t cw_len = static_cast<uint8_t>(extract_bits(bits, bit_offset, 1));
    plp.codeword_length =
        cw_len == 0 ? config::CodewordLength::SHORT : config::CodewordLength::LONG;
    bit_offset += 1;

    // Time interleaver depth (4 bits)
    plp.ti_depth = static_cast<uint8_t>(extract_bits(bits, bit_offset, 4));
    bit_offset += 4;

    // Time interleaver num blocks (10 bits)
    plp.ti_num_blocks = static_cast<uint16_t>(extract_bits(bits, bit_offset, 10));
    bit_offset += 10;

    // Skip reserved/other fields to match PLP_RECORD_BITS
    size_t fields_consumed = PLP_ID_BITS + PLP_TYPE_BITS + PLP_MOD_BITS + PLP_CODE_RATE_BITS +
                             PLP_TI_MODE_BITS + PLP_FEC_BLOCK_BITS + 1 + 4 + 10;
    if (fields_consumed < PLP_RECORD_BITS) {
        bit_offset += (PLP_RECORD_BITS - fields_consumed);
    }

    plp.valid = true;
    return plp;
}

L1Post L1Decoder::unpack_l1_post(const uint8_t* bits, size_t num_bits,
                                 uint8_t num_plps_hint) const {
    using namespace l1_post_fields;

    L1Post post;
    size_t bit_offset = 0;

    // Configurable section
    post.configurable.num_plps =
        static_cast<uint8_t>(extract_bits(bits, bit_offset + NUM_PLPS_OFFSET, NUM_PLPS_BITS));
    post.configurable.num_subframes = static_cast<uint8_t>(
        extract_bits(bits, bit_offset + NUM_SUBFRAMES_OFFSET, NUM_SUBFRAMES_BITS));

    // Move past configurable header
    bit_offset = 14;  // Skip header fields

    // Use hint if extracted value is invalid, otherwise use extracted value
    if (post.configurable.num_plps == 0 && num_plps_hint > 0) {
        post.configurable.num_plps = num_plps_hint;
    }

    // Unpack each PLP
    size_t plp_count = std::min(static_cast<size_t>(post.configurable.num_plps),
                                static_cast<size_t>(config::MAX_PLPS));

    for (size_t i = 0; i < plp_count; i++) {
        post.plps[i] = unpack_plp_config(bits, bit_offset);
    }

    post.size_bits = num_bits;
    post.crc_valid = validate_crc(bits, num_bits);

    return post;
}

bool L1Decoder::decode_l1_pre(const int8_t* llr_in, size_t num_bits, L1Pre& out) {
    if (llr_in == nullptr || num_bits == 0) {
        return false;
    }

    // FEC decode (LDPC + BCH)
    std::vector<uint8_t> decoded;
    if (!fec_decode(llr_in, num_bits, config_.l1_pre_short_ldpc, config::CodeRate::RATE_5_15,
                    decoded)) {
        return false;
    }

    // Unpack L1-Pre fields
    out = unpack_l1_pre(decoded.data(), decoded.size() * 8);

    return out.crc_valid || !config_.check_crc;
}

bool L1Decoder::decode_l1_post(const int8_t* llr_in, size_t num_bits, const L1Pre& l1_pre,
                               L1Post& out) {
    if (llr_in == nullptr || num_bits == 0) {
        return false;
    }

    // FEC decode using L1-Post parameters from L1-Pre
    bool short_ldpc = (l1_pre.l1_post_fec_type == 0);
    std::vector<uint8_t> decoded;
    if (!fec_decode(llr_in, num_bits, short_ldpc, l1_pre.l1_post_code_rate, decoded)) {
        return false;
    }

    // Unpack L1-Post fields
    out = unpack_l1_post(decoded.data(), decoded.size() * 8, l1_pre.num_subframes);

    return out.crc_valid || !config_.check_crc;
}

config::Atsc3Config L1Decoder::build_config(const L1Pre& pre, const L1Post& post) const {
    config::Atsc3Config config;

    // L1-Pre fields
    config.version = pre.version;
    config.fft_size = pre.fft_size;
    config.cp_length = pre.cp_length;
    config.pilot_pattern = pre.pilot_pattern;
    config.gi_fraction = pre.gi_fraction;
    config.preamble_reduced_carriers = pre.preamble_reduced_carriers;

    config.l1_post_size = pre.l1_post_size_cells;
    config.l1_post_modulation = pre.l1_post_modulation;
    config.l1_post_code_rate = pre.l1_post_code_rate;
    config.l1_post_scrambled = pre.l1_post_scrambled;

    // L1-Post fields
    config.num_subframes = post.configurable.num_subframes;
    config.num_plps = post.configurable.num_plps;

    // Copy PLP configurations
    size_t plp_count =
        std::min(static_cast<size_t>(config.num_plps), static_cast<size_t>(config::MAX_PLPS));
    for (size_t i = 0; i < plp_count; i++) {
        config.plps[i] = post.plps[i];
    }

    config.valid = (pre.crc_valid || !config_.check_crc) && (post.crc_valid || !config_.check_crc);

    return config;
}

}  // namespace framing
}  // namespace atsc3
