#pragma once

// BCH Decoder — ATSC 3.0 Bose-Chaudhuri-Hocquenghem decoding
//
// Implements BCH decoding for ATSC 3.0 over GF(2^16) with t=12 error
// correction capability. Used after LDPC decoding to correct residual
// errors in the information bits.
//
// Algorithm:
//   1. Compute 2t syndromes from received codeword
//   2. Berlekamp-Massey to find error locator polynomial
//   3. Chien search to find error positions
//   4. Correct errors by flipping bits at found positions
//
// AXI4-S Interface Contract:
//   Input:  TDATA=uint8_t (packed bits from LDPC decoder)
//   Output: TDATA=uint8_t (corrected information bits)
//           TVALID TREADY TLAST(codeword boundary)
//
// Reference: ATSC A/322 Section 10 (BCH Coding)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace fec {

// BCH decoder configuration
struct BchConfig {
    // ATSC 3.0 BCH parameters
    // Short frame: BCH parity over ~16200 LDPC info bits
    // Long frame: BCH parity over ~64800 LDPC info bits
    // t=12 error correction, m=16 (GF(2^16))

    // Codeword length in bits (LDPC info bits portion)
    size_t codeword_length = 57600;

    // Information length in bits (before BCH encoding)
    size_t info_length = 57408;

    // Error correction capability
    size_t t = 12;

    // GF(2^m) extension degree
    size_t m = 16;
};

// BCH decoding result
struct BchResult {
    // Decoded information bits (corrected)
    std::vector<uint8_t> decoded_bits;

    // Number of errors corrected
    size_t errors_corrected = 0;

    // True if decoding succeeded (errors <= t)
    bool success = false;

    // True if codeword was valid (no errors detected)
    bool was_valid = false;
};

// BCH Decoder
//
// Implements algebraic decoding using Berlekamp-Massey and Chien search.
// Pre-allocates GF tables at construction for efficient decoding.
//
// Usage:
//   BchConfig config;
//   config.t = 12;
//   BchDecoder decoder(config);
//
//   // Decode received bits
//   BchResult result = decoder.decode(received_bits, num_bits);
//   if (result.success) {
//       // Use result.decoded_bits
//   }
//
class BchDecoder {
public:
    explicit BchDecoder(const BchConfig& config = BchConfig());
    ~BchDecoder();

    // Decode BCH codeword
    // bits_in: Received codeword bits (packed, MSB first in each byte)
    // num_bits: Number of bits in codeword
    // Returns: Decoded information bits and status
    BchResult decode(const uint8_t* bits_in, size_t num_bits);

    // Decode with pre-allocated output buffer
    // bits_out: Output buffer for decoded bits (must have info_length/8 bytes)
    // Returns: Number of errors corrected, or -1 on failure
    int decode(const uint8_t* bits_in, size_t num_bits, uint8_t* bits_out);

    // Get configuration
    const BchConfig& get_config() const {
        return config_;
    }

    // Update configuration
    void set_config(const BchConfig& config);

    // Get parity bit count (m * t = 192 for ATSC 3.0)
    size_t parity_bits() const {
        return config_.m * config_.t;
    }

    // Check if decoder is initialized
    bool is_initialized() const {
        return initialized_;
    }

    // Reset internal state
    void reset();

private:
    BchConfig config_;
    bool initialized_ = false;

    // GF(2^16) arithmetic tables
    // gf_exp_[i] = α^i where α is primitive element
    // gf_log_[x] = i where α^i = x (undefined for x=0)
    std::vector<uint16_t> gf_exp_;
    std::vector<int32_t> gf_log_;

    // Working buffers (pre-allocated)
    std::vector<uint16_t> syndromes_;
    std::vector<uint16_t> error_locator_;
    std::vector<size_t> error_positions_;
    std::vector<uint8_t> working_bits_;

    // Initialize GF(2^16) tables
    // Uses primitive polynomial p(x) = x^16 + x^12 + x^3 + x + 1
    void init_gf_tables();

    // GF(2^16) arithmetic operations
    uint16_t gf_add(uint16_t a, uint16_t b) const {
        return a ^ b;  // Addition in GF(2^m) is XOR
    }

    uint16_t gf_mul(uint16_t a, uint16_t b) const;
    uint16_t gf_div(uint16_t a, uint16_t b) const;
    uint16_t gf_pow(uint16_t a, int n) const;
    uint16_t gf_inv(uint16_t a) const;

    // Compute 2t syndromes from received codeword
    // S_j = r(α^j) for j = 1, 2, ..., 2t
    void compute_syndromes(const uint8_t* bits, size_t num_bits);

    // Berlekamp-Massey algorithm to find error locator polynomial
    // Returns degree of error locator (number of errors)
    size_t berlekamp_massey();

    // Chien search to find error positions
    // Returns number of roots found (should equal degree of error locator)
    size_t chien_search(size_t num_bits);

    // Get bit at position from packed byte array
    bool get_bit(const uint8_t* bits, size_t pos) const {
        return (bits[pos / 8] >> (7 - (pos % 8))) & 1;
    }

    // Set bit at position in packed byte array
    void set_bit(uint8_t* bits, size_t pos, bool value) {
        if (value) {
            bits[pos / 8] |= (1 << (7 - (pos % 8)));
        } else {
            bits[pos / 8] &= ~(1 << (7 - (pos % 8)));
        }
    }

    // Flip bit at position
    void flip_bit(uint8_t* bits, size_t pos) {
        bits[pos / 8] ^= (1 << (7 - (pos % 8)));
    }
};

// Get BCH parameters for ATSC 3.0
// short_frame: true for 16200-bit LDPC, false for 64800-bit LDPC
BchConfig get_atsc3_bch_config(bool short_frame);

}  // namespace fec
}  // namespace atsc3
