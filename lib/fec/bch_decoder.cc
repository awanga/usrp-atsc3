// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// bch_decoder.cc — ATSC 3.0 BCH outer code (encoder and decoder) and CRC-32
//
// Reference: ATSC A/322 Section 6.1.2, Tables 6.1-6.3

#include "bch_decoder.h"

#include <algorithm>
#include <cstring>

namespace atsc3 {
namespace fec {

namespace {

// g1(x) of Table 6.3, the primitive polynomial that defines each field.
constexpr uint32_t kFieldPoly16 = 0x1002D;  // x^16 + x^5 + x^3 + x^2 + 1
constexpr uint32_t kFieldPoly14 = 0x402B;   // x^14 + x^5 + x^3 + x + 1

constexpr size_t kT = 12;

// Nouter = Kldpc per rate (Tables 6.1/6.2): Ninner * (rate + 2) / 15.
constexpr size_t kNinnerLong = 64800;
constexpr size_t kNinnerShort = 16200;

uint32_t field_poly(size_t m) {
    if (m == 16) {
        return kFieldPoly16;
    }
    if (m == 14) {
        return kFieldPoly14;
    }
    return 0;
}

bool valid_config(const BchConfig& c) {
    return field_poly(c.m) != 0 && c.t == kT && c.codeword_length > c.m * c.t &&
           c.info_length == c.codeword_length - c.m * c.t && c.codeword_length < (size_t{1} << c.m);
}

// Generator polynomial g(x) = product of the minimal polynomials of alpha^i,
// i = 1, 3, ..., 2t-1, in GF(2^m) defined by field_poly(m). Coefficients
// lowest power first, degree m*t.
std::vector<uint8_t> generator_polynomial(size_t m, size_t t) {
    const uint32_t order = (1u << m) - 1;
    const uint32_t poly = field_poly(m);
    std::vector<uint16_t> exp_table(order);
    uint32_t x = 1;
    for (uint32_t i = 0; i < order; i++) {
        exp_table[i] = static_cast<uint16_t>(x);
        x <<= 1;
        if (x & (1u << m)) {
            x ^= poly;
        }
    }
    std::vector<int32_t> log_table(order + 1, -1);
    for (uint32_t i = 0; i < order; i++) {
        log_table[exp_table[i]] = static_cast<int32_t>(i);
    }
    auto mul = [&](uint16_t a, uint16_t b) -> uint16_t {
        if (a == 0 || b == 0) {
            return 0;
        }
        return exp_table[(log_table[a] + log_table[b]) % order];
    };

    std::vector<uint8_t> g{1};
    std::vector<bool> used(order, false);
    for (uint32_t i = 1; i < 2 * t; i += 2) {
        if (used[i]) {
            continue;
        }
        // Minimal polynomial over the cyclotomic coset {i, 2i, 4i, ...}.
        std::vector<uint16_t> mp{1};
        uint32_t e = i;
        do {
            used[e] = true;
            uint16_t root = exp_table[e];
            std::vector<uint16_t> next(mp.size() + 1, 0);
            for (size_t k = 0; k < mp.size(); k++) {
                next[k + 1] ^= mp[k];
                next[k] ^= mul(mp[k], root);
            }
            mp.swap(next);
            e = (2 * e) % order;
        } while (e != i);

        std::vector<uint8_t> prod(g.size() + mp.size() - 1, 0);
        for (size_t a = 0; a < g.size(); a++) {
            if (!g[a]) {
                continue;
            }
            for (size_t b = 0; b < mp.size(); b++) {
                // Coefficients of a minimal polynomial are 0 or 1.
                prod[a + b] ^= static_cast<uint8_t>(mp[b] & 1);
            }
        }
        g.swap(prod);
    }
    return g;
}

}  // namespace

BchConfig get_atsc3_bch_config(config::CodeRate rate, bool short_frame) {
    BchConfig c;
    size_t ninner = short_frame ? kNinnerShort : kNinnerLong;
    size_t numerator = static_cast<size_t>(rate) + 2;
    c.m = short_frame ? 14 : 16;
    c.t = kT;
    c.codeword_length = ninner * numerator / 15;
    c.info_length = c.codeword_length - c.m * c.t;
    return c;
}

BchDecoder::BchDecoder(const BchConfig& config) {
    set_config(config);
}

BchDecoder::~BchDecoder() = default;

void BchDecoder::set_config(const BchConfig& config) {
    config_ = config;
    initialized_ = valid_config(config_);
    if (!initialized_) {
        return;
    }
    init_gf_tables();
    syndromes_.assign(2 * config_.t + 1, 0);
    error_locator_.assign(config_.t + 1, 0);
    error_positions_.clear();
    error_positions_.reserve(config_.t);
    working_bits_.assign(config_.codeword_length, 0);
}

void BchDecoder::reset() {
    std::fill(syndromes_.begin(), syndromes_.end(), 0);
    std::fill(error_locator_.begin(), error_locator_.end(), 0);
    error_positions_.clear();
}

void BchDecoder::init_gf_tables() {
    field_order_ = (1u << config_.m) - 1;
    const uint32_t poly = field_poly(config_.m);
    gf_exp_.assign(2 * static_cast<size_t>(field_order_), 0);
    gf_log_.assign(static_cast<size_t>(field_order_) + 1, -1);
    uint32_t x = 1;
    for (uint32_t i = 0; i < field_order_; i++) {
        gf_exp_[i] = static_cast<uint16_t>(x);
        gf_exp_[i + field_order_] = static_cast<uint16_t>(x);
        gf_log_[x] = static_cast<int32_t>(i);
        x <<= 1;
        if (x & (1u << config_.m)) {
            x ^= poly;
        }
    }
}

uint16_t BchDecoder::gf_mul(uint16_t a, uint16_t b) const {
    if (a == 0 || b == 0) {
        return 0;
    }
    return gf_exp_[static_cast<size_t>(gf_log_[a] + gf_log_[b])];
}

uint16_t BchDecoder::gf_div(uint16_t a, uint16_t b) const {
    if (a == 0 || b == 0) {
        return 0;
    }
    int32_t d = gf_log_[a] - gf_log_[b];
    if (d < 0) {
        d += static_cast<int32_t>(field_order_);
    }
    return gf_exp_[static_cast<size_t>(d)];
}

bool BchDecoder::compute_syndromes(const uint8_t* bits) {
    // S_j = r(alpha^j), r(x) = sum bits[i] x^(N-1-i): Horner from bit 0.
    // Only odd j are evaluated; S_2j = S_j^2 for a binary code.
    const size_t two_t = 2 * config_.t;
    const size_t n = config_.codeword_length;
    bool nonzero = false;
    for (size_t j = 1; j <= two_t; j += 2) {
        uint16_t s = 0;
        const int32_t step = static_cast<int32_t>(j);
        for (size_t i = 0; i < n; i++) {
            if (s != 0) {
                s = gf_exp_[static_cast<size_t>(gf_log_[s] + step)];
            }
            s ^= bits[i] & 1;
        }
        syndromes_[j] = s;
        nonzero = nonzero || s != 0;
    }
    for (size_t j = 2; j <= two_t; j += 2) {
        syndromes_[j] = gf_mul(syndromes_[j / 2], syndromes_[j / 2]);
    }
    return nonzero;
}

size_t BchDecoder::berlekamp_massey() {
    const size_t two_t = 2 * config_.t;
    const size_t len = config_.t + 1;
    // Working polynomials span 2t+1 terms so an over-degree locator is seen
    // (and rejected) rather than truncated.
    std::vector<uint16_t> sigma(two_t + 1, 0);
    std::vector<uint16_t> prev(two_t + 1, 0);
    std::vector<uint16_t> temp(two_t + 1, 0);
    sigma[0] = 1;
    prev[0] = 1;
    size_t l = 0;
    size_t shift = 1;
    uint16_t prev_delta = 1;

    for (size_t r = 1; r <= two_t; r++) {
        uint16_t delta = syndromes_[r];
        for (size_t i = 1; i <= l; i++) {
            delta ^= gf_mul(sigma[i], syndromes_[r - i]);
        }
        if (delta == 0) {
            shift++;
            continue;
        }
        temp = sigma;
        uint16_t coeff = gf_div(delta, prev_delta);
        for (size_t i = 0; i + shift <= two_t; i++) {
            sigma[i + shift] ^= gf_mul(coeff, prev[i]);
        }
        if (2 * l <= r - 1) {
            l = r - l;
            prev = temp;
            prev_delta = delta;
            shift = 1;
        } else {
            shift++;
        }
    }
    if (l < len) {
        std::copy(sigma.begin(), sigma.begin() + static_cast<std::ptrdiff_t>(len),
                  error_locator_.begin());
    }
    return l;
}

size_t BchDecoder::chien_search() {
    // Bit i is the coefficient of x^p with p = N-1-i; it is in error iff
    // sigma(alpha^-p) = 0. Term k of sigma(alpha^-p) is sigma_k * alpha^(-k p).
    error_positions_.clear();
    size_t degree = 0;
    for (size_t k = 0; k <= config_.t; k++) {
        if (error_locator_[k] != 0) {
            degree = k;
        }
    }
    const size_t n = config_.codeword_length;
    std::vector<uint16_t> term(error_locator_.begin(),
                               error_locator_.begin() + static_cast<std::ptrdiff_t>(degree + 1));
    for (size_t p = 0; p < n; p++) {
        uint16_t sum = 0;
        for (size_t k = 0; k <= degree; k++) {
            sum ^= term[k];
        }
        if (sum == 0) {
            error_positions_.push_back(n - 1 - p);
            if (error_positions_.size() == degree) {
                break;
            }
        }
        for (size_t k = 1; k <= degree; k++) {
            if (term[k] != 0) {
                int32_t e = gf_log_[term[k]] - static_cast<int32_t>(k);
                if (e < 0) {
                    e += static_cast<int32_t>(field_order_);
                }
                term[k] = gf_exp_[static_cast<size_t>(e)];
            }
        }
    }
    return error_positions_.size();
}

BchResult BchDecoder::decode(const uint8_t* bits_in, size_t num_bits) {
    BchResult result;
    if (!initialized_ || bits_in == nullptr || num_bits != config_.codeword_length) {
        return result;
    }
    std::memcpy(working_bits_.data(), bits_in, num_bits);

    if (!compute_syndromes(working_bits_.data())) {
        result.was_valid = true;
        result.success = true;
    } else {
        size_t num_errors = berlekamp_massey();
        if (num_errors > config_.t || chien_search() != num_errors) {
            return result;
        }
        for (size_t pos : error_positions_) {
            working_bits_[pos] ^= 1;
        }
        result.success = true;
        result.errors_corrected = num_errors;
    }
    result.decoded_bits.assign(working_bits_.begin(),
                               working_bits_.begin() +
                                   static_cast<std::ptrdiff_t>(config_.info_length));
    return result;
}

int BchDecoder::decode(const uint8_t* bits_in, size_t num_bits, uint8_t* bits_out) {
    if (bits_out == nullptr) {
        return -1;
    }
    BchResult result = decode(bits_in, num_bits);
    if (!result.success) {
        return -1;
    }
    std::memcpy(bits_out, result.decoded_bits.data(), result.decoded_bits.size());
    return static_cast<int>(result.errors_corrected);
}

bool bch_encode(const BchConfig& config, const uint8_t* payload, uint8_t* codeword) {
    if (!valid_config(config) || payload == nullptr || codeword == nullptr) {
        return false;
    }
    const size_t mouter = config.m * config.t;
    const std::vector<uint8_t> g = generator_polynomial(config.m, config.t);

    // LFSR division of payload(x) * x^Mouter by g(x); reg[k] holds the
    // coefficient of x^k of the running remainder.
    std::vector<uint8_t> reg(mouter, 0);
    for (size_t i = 0; i < config.info_length; i++) {
        uint8_t feedback = static_cast<uint8_t>((payload[i] & 1) ^ reg[mouter - 1]);
        for (size_t k = mouter - 1; k > 0; k--) {
            reg[k] = static_cast<uint8_t>(reg[k - 1] ^ (feedback & g[k]));
        }
        reg[0] = feedback;
        codeword[i] = payload[i] & 1;
    }
    for (size_t k = 0; k < mouter; k++) {
        codeword[config.info_length + k] = reg[mouter - 1 - k];
    }
    return true;
}

uint32_t outer_crc32(const uint8_t* bits, size_t num_bits) {
    constexpr uint32_t kPoly = (1u << 21) | (1u << 16) | (1u << 11) | 1u;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < num_bits; i++) {
        uint32_t feedback = (crc >> 31) ^ (bits[i] & 1u);
        crc <<= 1;
        if (feedback) {
            crc ^= kPoly;
        }
    }
    return crc;
}

}  // namespace fec
}  // namespace atsc3
