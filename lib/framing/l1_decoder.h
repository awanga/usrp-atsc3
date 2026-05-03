#pragma once

// L1 Preamble Decoder — ATSC 3.0 Layer 1 signaling extraction
//
// Decodes L1-Pre and L1-Post from preamble symbols to configure
// all downstream processing blocks. Uses LDPC + BCH for FEC.
//
// L1-Pre (from bootstrap):
//   - FFT size, CP length, pilot pattern
//   - L1-Post configuration (size, modulation, code rate)
//
// L1-Post (from preamble symbols):
//   - Number of subframes and PLPs
//   - Per-PLP configuration (modulation, code rate, interleaving)
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8_t (soft LLRs from demapper)
//   Output: Atsc3Config struct via observer pattern
//           TVALID on successful decode
//
// Reference: ATSC A/322 Section 5 (L1 Signaling)

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace framing {

// L1-Pre structure (parsed from bootstrap OFDM symbols)
// Contains OFDM configuration and L1-Post format
struct L1Pre {
    // L1-Pre version (0 = initial ATSC 3.0)
    uint8_t version = 0;

    // OFDM configuration
    config::FftSize fft_size = config::FftSize::FFT_8K;
    uint8_t gi_fraction = 0;  // Guard interval index
    config::CpLength cp_length = config::CpLength::CP_1024_8192;
    config::PilotPattern pilot_pattern = config::PilotPattern::PP3;

    // L1-Post configuration
    uint16_t l1_post_size_cells = 0;
    config::Modulation l1_post_modulation = config::Modulation::QPSK;
    config::CodeRate l1_post_code_rate = config::CodeRate::RATE_5_15;
    bool l1_post_scrambled = false;
    uint8_t l1_post_fec_type = 0;  // 0 = BCH + 16K LDPC, 1 = BCH + 64K LDPC

    // Frame configuration
    uint8_t num_subframes = 1;
    bool preamble_reduced_carriers = false;

    // CRC status
    bool crc_valid = false;

    // Raw data for debugging
    static constexpr size_t L1_PRE_BITS = 200;  // Approximate L1-Pre size
};

// L1-Post configurable block structure
struct L1PostConfigurable {
    uint8_t num_plps = 0;
    uint8_t num_subframes = 1;
    bool time_info_present = false;
    bool reserved = false;
};

// L1-Post structure (parsed from preamble symbols)
// Contains PLP configurations for the current frame
struct L1Post {
    // Configurable parameters
    L1PostConfigurable configurable;

    // PLP configurations
    std::array<config::PlpConfig, config::MAX_PLPS> plps;

    // Dynamic parameters (can change per-frame)
    uint16_t frame_index = 0;
    uint8_t l1_change_counter = 0;

    // CRC status
    bool crc_valid = false;

    // Size in bits (varies with num_plps)
    size_t size_bits = 0;
};

// L1 Decoder configuration
struct L1DecoderConfig {
    // Maximum LDPC iterations for L1 decoding
    size_t max_ldpc_iterations = 25;

    // Enable CRC checking (can be disabled for testing)
    bool check_crc = true;

    // L1-Pre LDPC uses short (16200) codeword
    bool l1_pre_short_ldpc = true;
};

// Observer callback type for configuration updates
using ConfigObserver = std::function<void(const config::Atsc3Config&)>;

// L1 Preamble Decoder
//
// Decodes L1-Pre and L1-Post signaling from OFDM preamble.
// Uses internal LDPC + BCH decoders for error correction.
//
// Usage:
//   L1Decoder decoder;
//
//   // Decode L1-Pre from bootstrap
//   L1Pre l1_pre;
//   if (decoder.decode_l1_pre(llr_data, num_bits, l1_pre)) {
//       // L1-Pre valid, now decode L1-Post
//       L1Post l1_post;
//       if (decoder.decode_l1_post(llr_data2, num_bits2, l1_pre, l1_post)) {
//           // Build full configuration
//           auto config = decoder.build_config(l1_pre, l1_post);
//       }
//   }
//
class L1Decoder {
public:
    explicit L1Decoder(const L1DecoderConfig& config = L1DecoderConfig());
    ~L1Decoder();

    // Decode L1-Pre from bootstrap symbols
    // llr_in: Soft LLRs from demapper
    // num_bits: Number of bits (including parity)
    // out: Parsed L1-Pre structure
    // Returns: true if decode and CRC successful
    bool decode_l1_pre(const int8_t* llr_in, size_t num_bits, L1Pre& out);

    // Decode L1-Post from preamble symbols
    // llr_in: Soft LLRs from demapper
    // num_bits: Number of bits (including parity)
    // l1_pre: Previously decoded L1-Pre (for configuration)
    // out: Parsed L1-Post structure
    // Returns: true if decode and CRC successful
    bool decode_l1_post(const int8_t* llr_in, size_t num_bits, const L1Pre& l1_pre, L1Post& out);

    // Build full Atsc3Config from decoded L1-Pre and L1-Post
    config::Atsc3Config build_config(const L1Pre& pre, const L1Post& post) const;

    // Register observer for configuration updates
    void add_observer(ConfigObserver observer);

    // Notify observers of new configuration
    void notify_observers(const config::Atsc3Config& config);

    // Get decoder configuration
    const L1DecoderConfig& get_config() const {
        return config_;
    }

    // Reset decoder state
    void reset();

private:
    L1DecoderConfig config_;
    std::vector<ConfigObserver> observers_;

    // Working buffers
    std::vector<uint8_t> decoded_bits_;
    std::vector<int8_t> llr_buffer_;

    // CRC-32 computation (ATSC 3.0 uses CRC-32/ISO-3309)
    uint32_t compute_crc32(const uint8_t* data, size_t num_bits) const;

    // Validate CRC at end of L1 block
    bool validate_crc(const uint8_t* data, size_t num_bits) const;

    // FEC decoding (LDPC + BCH)
    bool fec_decode(const int8_t* llr_in, size_t num_bits, bool short_ldpc,
                    config::CodeRate code_rate, std::vector<uint8_t>& out);

    // Bit extraction helpers
    uint32_t extract_bits(const uint8_t* data, size_t bit_offset, size_t num_bits) const;

    // Unpack L1-Pre from decoded bits
    L1Pre unpack_l1_pre(const uint8_t* bits, size_t num_bits) const;

    // Unpack L1-Post from decoded bits
    L1Post unpack_l1_post(const uint8_t* bits, size_t num_bits, uint8_t num_plps) const;

    // Unpack single PLP configuration
    config::PlpConfig unpack_plp_config(const uint8_t* bits, size_t& bit_offset) const;
};

// L1-Pre bit field definitions per ATSC A/322 Table 5.1
namespace l1_pre_fields {
constexpr size_t VERSION_OFFSET = 0;
constexpr size_t VERSION_BITS = 3;

constexpr size_t FFT_SIZE_OFFSET = 3;
constexpr size_t FFT_SIZE_BITS = 2;

constexpr size_t GI_FRACTION_OFFSET = 5;
constexpr size_t GI_FRACTION_BITS = 4;

constexpr size_t PILOT_PATTERN_OFFSET = 9;
constexpr size_t PILOT_PATTERN_BITS = 3;

constexpr size_t L1_POST_SIZE_OFFSET = 12;
constexpr size_t L1_POST_SIZE_BITS = 16;

constexpr size_t L1_POST_MOD_OFFSET = 28;
constexpr size_t L1_POST_MOD_BITS = 4;

constexpr size_t L1_POST_CODE_RATE_OFFSET = 32;
constexpr size_t L1_POST_CODE_RATE_BITS = 4;

constexpr size_t L1_POST_FEC_TYPE_OFFSET = 36;
constexpr size_t L1_POST_FEC_TYPE_BITS = 2;

constexpr size_t PREAMBLE_REDUCED_OFFSET = 38;
constexpr size_t PREAMBLE_REDUCED_BITS = 1;

constexpr size_t NUM_SUBFRAMES_OFFSET = 39;
constexpr size_t NUM_SUBFRAMES_BITS = 8;

constexpr size_t CRC_OFFSET = 168;  // CRC starts near end
constexpr size_t CRC_BITS = 32;

// Total L1-Pre information bits (before FEC)
constexpr size_t INFO_BITS = 200;
}  // namespace l1_pre_fields

// L1-Post configurable bit field definitions
namespace l1_post_fields {
constexpr size_t NUM_PLPS_OFFSET = 0;
constexpr size_t NUM_PLPS_BITS = 6;

constexpr size_t NUM_SUBFRAMES_OFFSET = 6;
constexpr size_t NUM_SUBFRAMES_BITS = 8;

// Per-PLP fields (repeated num_plps times)
constexpr size_t PLP_ID_BITS = 6;
constexpr size_t PLP_TYPE_BITS = 1;
constexpr size_t PLP_MOD_BITS = 4;
constexpr size_t PLP_CODE_RATE_BITS = 4;
constexpr size_t PLP_TI_MODE_BITS = 2;
constexpr size_t PLP_FEC_BLOCK_BITS = 15;

// Size of each PLP record in bits
constexpr size_t PLP_RECORD_BITS = 90;  // Approximate
}  // namespace l1_post_fields

}  // namespace framing
}  // namespace atsc3
