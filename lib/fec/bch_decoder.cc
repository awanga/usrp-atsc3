// bch_decoder.cc — ATSC 3.0 BCH decoding using Berlekamp-Massey algorithm
//
// Implements algebraic BCH decoding over GF(2^16) with t=12 error correction.
//
// Reference: ATSC A/322 Section 10 (BCH Coding)

#include "bch_decoder.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace atsc3 {
namespace fec {

namespace {

// ATSC 3.0 BCH primitive polynomial for GF(2^16)
// p(x) = x^16 + x^12 + x^3 + x + 1
// Binary: 0x1100B = 0001 0001 0000 0000 1011
constexpr uint32_t GF_PRIMITIVE_POLY = 0x1100B;

// Field size
constexpr size_t GF_SIZE = 65536;   // 2^16
constexpr size_t GF_ORDER = 65535;  // 2^16 - 1

}  // namespace

BchConfig get_atsc3_bch_config(bool short_frame) {
    BchConfig config;
    config.m = 16;
    config.t = 12;

    if (short_frame) {
        // Short frame: 16200-bit LDPC codeword
        // BCH protects the information bits
        config.codeword_length = 14400;
        config.info_length = 14400 - 192;  // 192 = m*t parity bits
    } else {
        // Long frame: 64800-bit LDPC codeword
        config.codeword_length = 57600;
        config.info_length = 57600 - 192;
    }

    return config;
}

BchDecoder::BchDecoder(const BchConfig& config) : config_(config) {
    init_gf_tables();

    // Pre-allocate working buffers
    syndromes_.resize(2 * config_.t + 1);
    error_locator_.resize(config_.t + 2);
    error_positions_.reserve(config_.t);
    working_bits_.resize((config_.codeword_length + 7) / 8);

    initialized_ = true;
}

BchDecoder::~BchDecoder() = default;

void BchDecoder::set_config(const BchConfig& config) {
    config_ = config;

    // Reallocate buffers if needed
    syndromes_.resize(2 * config_.t + 1);
    error_locator_.resize(config_.t + 2);
    error_positions_.reserve(config_.t);
    working_bits_.resize((config_.codeword_length + 7) / 8);
}

void BchDecoder::reset() {
    std::fill(syndromes_.begin(), syndromes_.end(), 0);
    std::fill(error_locator_.begin(), error_locator_.end(), 0);
    error_positions_.clear();
}

void BchDecoder::init_gf_tables() {
    // Initialize GF(2^16) exponentiation and logarithm tables
    // α is the primitive element (root of primitive polynomial)

    gf_exp_.resize(2 * GF_ORDER);  // Double size for easy modular access
    gf_log_.resize(GF_SIZE);

    // gf_log_[0] is undefined, set to -1
    gf_log_[0] = -1;

    uint32_t x = 1;
    for (size_t i = 0; i < GF_ORDER; i++) {
        gf_exp_[i] = static_cast<uint16_t>(x);
        gf_exp_[i + GF_ORDER] = static_cast<uint16_t>(x);  // Duplicate for wrap
        gf_log_[x] = static_cast<int32_t>(i);

        // Multiply by α (shift left, reduce by primitive polynomial if overflow)
        x <<= 1;
        if (x & 0x10000) {
            x ^= GF_PRIMITIVE_POLY;
        }
    }
}

uint16_t BchDecoder::gf_mul(uint16_t a, uint16_t b) const {
    if (a == 0 || b == 0) {
        return 0;
    }
    // α^i * α^j = α^(i+j mod order)
    int32_t sum = gf_log_[a] + gf_log_[b];
    return gf_exp_[sum % GF_ORDER];
}

uint16_t BchDecoder::gf_div(uint16_t a, uint16_t b) const {
    if (b == 0) {
        return 0;  // Division by zero - return 0
    }
    if (a == 0) {
        return 0;
    }
    // α^i / α^j = α^(i-j mod order)
    int32_t diff = gf_log_[a] - gf_log_[b];
    if (diff < 0) {
        diff += GF_ORDER;
    }
    return gf_exp_[diff];
}

uint16_t BchDecoder::gf_pow(uint16_t a, int n) const {
    if (a == 0) {
        return 0;
    }
    if (n == 0) {
        return 1;
    }
    // α^(i*n mod order)
    int64_t exp = (static_cast<int64_t>(gf_log_[a]) * n) % GF_ORDER;
    if (exp < 0) {
        exp += GF_ORDER;
    }
    return gf_exp_[exp];
}

uint16_t BchDecoder::gf_inv(uint16_t a) const {
    if (a == 0) {
        return 0;  // Inverse of 0 is undefined
    }
    // α^(-i) = α^(order - i)
    return gf_exp_[GF_ORDER - gf_log_[a]];
}

void BchDecoder::compute_syndromes(const uint8_t* bits, size_t num_bits) {
    // Compute syndromes S_j = r(α^j) for j = 1, 2, ..., 2t
    // where r(x) = r_0 + r_1*x + r_2*x^2 + ... + r_(n-1)*x^(n-1)
    // and r_i is the i-th received bit

    size_t two_t = 2 * config_.t;

    for (size_t j = 1; j <= two_t; j++) {
        uint16_t syndrome = 0;

        // Evaluate polynomial at α^j
        // Use Horner's method: r(α^j) = r_0 + α^j(r_1 + α^j(r_2 + ...))
        // But for BCH it's simpler to sum directly
        for (size_t i = 0; i < num_bits; i++) {
            if (get_bit(bits, i)) {
                // Add α^(j*i) to syndrome
                size_t exp = (j * i) % GF_ORDER;
                syndrome = gf_add(syndrome, gf_exp_[exp]);
            }
        }

        syndromes_[j] = syndrome;
    }
}

size_t BchDecoder::berlekamp_massey() {
    // Berlekamp-Massey algorithm to find error locator polynomial
    // Returns the degree of the error locator (number of errors)

    size_t two_t = 2 * config_.t;

    // σ(x) = 1 + σ_1*x + σ_2*x^2 + ... (error locator polynomial)
    std::vector<uint16_t> sigma(config_.t + 2, 0);
    std::vector<uint16_t> sigma_prev(config_.t + 2, 0);
    std::vector<uint16_t> sigma_temp(config_.t + 2, 0);

    sigma[0] = 1;
    sigma_prev[0] = 1;

    size_t L = 0;  // Current number of errors
    int m = 1;     // Step since last length change
    uint16_t delta_prev = 1;

    for (size_t n = 1; n <= two_t; n++) {
        // Compute discrepancy delta_n
        uint16_t delta = syndromes_[n];
        for (size_t i = 1; i <= L; i++) {
            if (sigma[i] != 0 && syndromes_[n - i] != 0) {
                delta = gf_add(delta, gf_mul(sigma[i], syndromes_[n - i]));
            }
        }

        if (delta == 0) {
            // No change to σ(x)
            m++;
        } else {
            // σ_temp(x) = σ(x)
            std::copy(sigma.begin(), sigma.end(), sigma_temp.begin());

            // σ(x) = σ(x) - (delta/delta_prev) * x^m * σ_prev(x)
            uint16_t coeff = gf_div(delta, delta_prev);
            for (size_t i = 0; i + m <= config_.t + 1; i++) {
                if (sigma_prev[i] != 0) {
                    sigma[i + m] = gf_add(sigma[i + m], gf_mul(coeff, sigma_prev[i]));
                }
            }

            if (2 * L <= n - 1) {
                L = n - L;
                std::copy(sigma_temp.begin(), sigma_temp.end(), sigma_prev.begin());
                delta_prev = delta;
                m = 1;
            } else {
                m++;
            }
        }
    }

    // Copy result to error_locator_
    std::copy(sigma.begin(), sigma.begin() + L + 1, error_locator_.begin());

    return L;
}

size_t BchDecoder::chien_search(size_t num_bits) {
    // Chien search: find roots of error locator polynomial
    // If σ(α^(-i)) = 0, then position i has an error

    error_positions_.clear();

    size_t degree = 0;
    for (size_t i = 0; i <= config_.t; i++) {
        if (error_locator_[i] != 0) {
            degree = i;
        }
    }

    if (degree == 0) {
        // No errors
        return 0;
    }

    // Evaluate σ(α^(-i)) for i = 0, 1, ..., num_bits-1
    // Using iterative evaluation for efficiency
    std::vector<uint16_t> eval(degree + 1);
    for (size_t j = 0; j <= degree; j++) {
        eval[j] = error_locator_[j];
    }

    for (size_t i = 0; i < num_bits; i++) {
        // Evaluate σ at α^(-i)
        uint16_t sum = eval[0];
        for (size_t j = 1; j <= degree; j++) {
            sum = gf_add(sum, eval[j]);
        }

        if (sum == 0) {
            // Found an error at position i
            error_positions_.push_back(i);

            if (error_positions_.size() == degree) {
                // Found all roots
                break;
            }
        }

        // Update for next position: multiply each term by α^(-j)
        for (size_t j = 1; j <= degree; j++) {
            // eval[j] *= α^(-j)
            if (eval[j] != 0) {
                int32_t new_exp = gf_log_[eval[j]] - static_cast<int32_t>(j);
                if (new_exp < 0) {
                    new_exp += GF_ORDER;
                }
                eval[j] = gf_exp_[new_exp];
            }
        }
    }

    return error_positions_.size();
}

BchResult BchDecoder::decode(const uint8_t* bits_in, size_t num_bits) {
    BchResult result;

    if (bits_in == nullptr || num_bits == 0) {
        return result;
    }

    // Limit to codeword length
    if (num_bits > config_.codeword_length) {
        num_bits = config_.codeword_length;
    }

    // Copy input to working buffer
    size_t num_bytes = (num_bits + 7) / 8;
    std::memcpy(working_bits_.data(), bits_in, num_bytes);

    // Compute syndromes
    compute_syndromes(working_bits_.data(), num_bits);

    // Check if all syndromes are zero (no errors)
    bool all_zero = true;
    for (size_t j = 1; j <= 2 * config_.t; j++) {
        if (syndromes_[j] != 0) {
            all_zero = false;
            break;
        }
    }

    if (all_zero) {
        // Codeword is valid, no errors
        result.was_valid = true;
        result.success = true;
        result.errors_corrected = 0;

        // Copy info bits to output
        size_t info_bytes = (config_.info_length + 7) / 8;
        result.decoded_bits.resize(info_bytes);
        std::memcpy(result.decoded_bits.data(), working_bits_.data(), info_bytes);

        return result;
    }

    // Find error locator polynomial using Berlekamp-Massey
    size_t num_errors = berlekamp_massey();

    if (num_errors > config_.t) {
        // Too many errors to correct
        result.success = false;
        result.errors_corrected = 0;
        return result;
    }

    // Find error positions using Chien search
    size_t roots_found = chien_search(num_bits);

    if (roots_found != num_errors) {
        // Number of roots doesn't match degree - decoding failure
        result.success = false;
        result.errors_corrected = 0;
        return result;
    }

    // Correct errors by flipping bits
    for (size_t pos : error_positions_) {
        if (pos < num_bits) {
            flip_bit(working_bits_.data(), pos);
        }
    }

    // Verify correction by recomputing syndromes
    compute_syndromes(working_bits_.data(), num_bits);
    all_zero = true;
    for (size_t j = 1; j <= 2 * config_.t; j++) {
        if (syndromes_[j] != 0) {
            all_zero = false;
            break;
        }
    }

    if (!all_zero) {
        // Correction failed (shouldn't happen if algorithm is correct)
        result.success = false;
        result.errors_corrected = 0;
        return result;
    }

    // Copy corrected info bits to output
    size_t info_bytes = (config_.info_length + 7) / 8;
    result.decoded_bits.resize(info_bytes);
    std::memcpy(result.decoded_bits.data(), working_bits_.data(), info_bytes);

    result.success = true;
    result.was_valid = false;
    result.errors_corrected = error_positions_.size();

    return result;
}

int BchDecoder::decode(const uint8_t* bits_in, size_t num_bits, uint8_t* bits_out) {
    BchResult result = decode(bits_in, num_bits);

    if (!result.success) {
        return -1;
    }

    if (bits_out != nullptr && !result.decoded_bits.empty()) {
        std::memcpy(bits_out, result.decoded_bits.data(), result.decoded_bits.size());
    }

    return static_cast<int>(result.errors_corrected);
}

}  // namespace fec
}  // namespace atsc3
