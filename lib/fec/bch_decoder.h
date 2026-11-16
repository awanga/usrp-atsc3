// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// BCH outer code — ATSC 3.0 (A/322 Section 6.1.2.1, Table 6.3)
//
// 12-error-correcting shortened binary BCH codes:
//   Ninner = 64800: GF(2^16), field polynomial g1(x) = x^16+x^5+x^3+x^2+1,
//                   Mouter = 192 parity bits
//   Ninner = 16200: GF(2^14), field polynomial g1(x) = x^14+x^5+x^3+x+1,
//                   Mouter = 168 parity bits
// The codeword is the Nouter LDPC information bits (Tables 6.1/6.2): the
// Kpayload Baseband Packet bits followed by the parity. Bit i of the
// codeword is the coefficient of x^(Nouter-1-i).
//
// g(x) = g1(x)...g12(x), the minimal polynomials of alpha, alpha^3, ...,
// alpha^23, so a codeword has alpha^1..alpha^24 as roots. The decoder computes
// those 24 syndromes, finds the error locator with Berlekamp-Massey and its
// roots with a Chien search over the Nouter positions of the shortened code.
//
// Bits are one per byte (0/1), the format the LDPC decoder emits.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=uint8 (one codeword bit) TVALID TREADY TLAST(codeword)
//   Output: TDATA=uint8 (one payload bit)  TVALID TREADY TLAST(payload)

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace fec {

struct BchConfig {
    // Nouter: codeword length in bits (LDPC information bits)
    size_t codeword_length = 0;
    // Kpayload: Baseband Packet bits protected by the codeword
    size_t info_length = 0;
    // Error correction capability
    size_t t = 12;
    // GF(2^m): 16 for 64800-bit LDPC codes, 14 for 16200-bit codes
    size_t m = 16;
};

struct BchResult {
    // Corrected payload bits (info_length), one per byte
    std::vector<uint8_t> decoded_bits;
    size_t errors_corrected = 0;
    // True if the codeword was corrected (errors <= t) or already valid
    bool success = false;
    // True if every syndrome was zero on input
    bool was_valid = false;
};

// BCH parameters of A/322 Tables 6.1/6.2 and 6.3 for one LDPC code.
BchConfig get_atsc3_bch_config(config::CodeRate rate, bool short_frame);

class BchDecoder {
public:
    // config must come from get_atsc3_bch_config() or match it; otherwise
    // is_initialized() is false and decode() fails.
    explicit BchDecoder(const BchConfig& config);
    ~BchDecoder();

    // bits_in: num_bits codeword bits; num_bits must equal codeword_length.
    BchResult decode(const uint8_t* bits_in, size_t num_bits);

    // bits_out: info_length bits. Returns the number of errors corrected, or
    // -1 if the codeword is uncorrectable or the arguments are invalid.
    int decode(const uint8_t* bits_in, size_t num_bits, uint8_t* bits_out);

    const BchConfig& get_config() const {
        return config_;
    }

    void set_config(const BchConfig& config);

    size_t parity_bits() const {
        return config_.m * config_.t;
    }

    bool is_initialized() const {
        return initialized_;
    }

    void reset();

private:
    BchConfig config_;
    bool initialized_ = false;
    uint32_t field_order_ = 0;  // 2^m - 1

    std::vector<uint16_t> gf_exp_;  // gf_exp_[i] = alpha^i, doubled for wrap
    std::vector<int32_t> gf_log_;   // gf_log_[x] = i with alpha^i = x; -1 for 0

    std::vector<uint16_t> syndromes_;      // [1..2t]
    std::vector<uint16_t> error_locator_;  // sigma_0..sigma_t
    std::vector<size_t> error_positions_;  // codeword bit indices
    std::vector<uint8_t> working_bits_;

    void init_gf_tables();
    uint16_t gf_mul(uint16_t a, uint16_t b) const;
    uint16_t gf_div(uint16_t a, uint16_t b) const;
    // Returns true if any syndrome is nonzero.
    bool compute_syndromes(const uint8_t* bits);
    size_t berlekamp_massey();
    size_t chien_search();
};

// Systematic encoder of A/322 Section 6.1.2.1 (the transmitter's procedure):
// codeword = payload followed by the remainder of payload(x)*x^Mouter / g(x).
// payload: info_length bits; codeword: codeword_length bits; one per byte.
// Returns false if config is not an A/322 BCH configuration.
bool bch_encode(const BchConfig& config, const uint8_t* payload, uint8_t* codeword);

// CRC-32 outer code of A/322 Section 6.1.2.2: G(x) = x^32+x^21+x^16+x^11+1,
// register initialised to all ones, data MSB first, no final inversion.
// Returns the 32 parity bits b31..b0 packed MSB first.
uint32_t outer_crc32(const uint8_t* bits, size_t num_bits);

}  // namespace fec
}  // namespace atsc3
