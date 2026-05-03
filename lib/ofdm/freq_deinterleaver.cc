// freq_deinterleaver.cc — ATSC 3.0 frequency-domain de-interleaving
//
// Reverses frequency interleaving to recover original carrier ordering.
//
// Reference: ATSC A/322 Section 8.3 (Frequency Interleaving)

#include "freq_deinterleaver.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

namespace {

// Find the smallest power of 2 >= n
inline size_t next_power_of_two(size_t n) {
    if (n == 0)
        return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// Count bits needed to represent n-1
inline size_t count_bits(size_t n) {
    size_t count = 0;
    while (n > 0) {
        n >>= 1;
        ++count;
    }
    return count;
}

// Bit-reversal of an n-bit number
inline size_t bit_reverse(size_t x, size_t num_bits) {
    size_t result = 0;
    for (size_t i = 0; i < num_bits; ++i) {
        result = (result << 1) | (x & 1);
        x >>= 1;
    }
    return result;
}

}  // namespace

FreqDeinterleaver::FreqDeinterleaver(const FreqDeinterleaverConfig& config) : config_(config) {
    compute_permutation();
}

FreqDeinterleaver::~FreqDeinterleaver() = default;

void FreqDeinterleaver::compute_permutation() {
    size_t n = config_.num_active_carriers;
    if (n == 0) {
        inv_permutation_.clear();
        temp_buffer_.clear();
        return;
    }

    // Use bit-reversal permutation based on carrier count
    // Same algorithm as cell deinterleaver for reliable bijection
    size_t n_padded = next_power_of_two(n);
    size_t num_bits = count_bits(n_padded - 1);
    if (num_bits == 0)
        num_bits = 1;

    std::vector<size_t> fwd_perm(n);

    if (n == n_padded) {
        // Power-of-2: simple bit reversal
        for (size_t i = 0; i < n; ++i) {
            fwd_perm[i] = bit_reverse(i, num_bits);
        }
    } else {
        // Non-power-of-2: use valid-pair matching algorithm
        std::vector<std::pair<size_t, size_t>> valid_pairs;
        for (size_t i = 0; i < n_padded; ++i) {
            size_t rev = bit_reverse(i, num_bits);
            if (i < n && rev < n) {
                valid_pairs.push_back({i, rev});
            }
        }

        std::vector<bool> src_used(n, false);
        std::vector<bool> dst_used(n, false);

        for (const auto& p : valid_pairs) {
            size_t src = p.first;
            size_t dst = p.second;
            if (!src_used[src] && !dst_used[dst]) {
                fwd_perm[src] = dst;
                src_used[src] = true;
                dst_used[dst] = true;
            }
        }

        // Fill remaining positions
        std::vector<size_t> unused_dst;
        for (size_t i = 0; i < n; ++i) {
            if (!dst_used[i]) {
                unused_dst.push_back(i);
            }
        }

        size_t unused_idx = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!src_used[i]) {
                fwd_perm[i] = unused_dst[unused_idx++];
            }
        }
    }

    // Compute mathematical inverse for de-interleaving
    inv_permutation_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        inv_permutation_[fwd_perm[i]] = i;
    }

    // Pre-allocate temp buffer
    temp_buffer_.resize(n);
}

size_t FreqDeinterleaver::interleaver_permutation(size_t input_pos) const {
    if (input_pos >= config_.num_active_carriers) {
        return input_pos;
    }

    // Return the forward permutation value
    // This is the inverse of inv_permutation_
    // Since fwd[i] = j means inv[j] = i, we need to find j where inv[j] = input_pos
    for (size_t j = 0; j < inv_permutation_.size(); ++j) {
        if (inv_permutation_[j] == input_pos) {
            return j;
        }
    }

    return input_pos;
}

void FreqDeinterleaver::deinterleave(const int8_t* input, int8_t* output) const {
    if (input == nullptr || output == nullptr) {
        return;
    }

    size_t n = config_.num_active_carriers;
    if (n == 0 || inv_permutation_.size() != n) {
        return;
    }

    // Apply inverse permutation
    for (size_t i = 0; i < n; ++i) {
        output[i] = input[inv_permutation_[i]];
    }
}

void FreqDeinterleaver::deinterleave_inplace(int8_t* carriers) {
    if (carriers == nullptr) {
        return;
    }

    size_t n = config_.num_active_carriers;
    if (n == 0 || inv_permutation_.size() != n) {
        return;
    }

    // Copy to temp buffer
    std::copy(carriers, carriers + n, temp_buffer_.begin());

    // Apply inverse permutation
    for (size_t i = 0; i < n; ++i) {
        carriers[i] = temp_buffer_[inv_permutation_[i]];
    }
}

void FreqDeinterleaver::set_config(const FreqDeinterleaverConfig& config) {
    config_ = config;
    compute_permutation();
}

void FreqDeinterleaver::reset() {
    // No state to reset beyond permutation tables
}

size_t compute_freq_interleaver_position(size_t input_pos, size_t fft_size, size_t num_carriers) {
    if (input_pos >= num_carriers || num_carriers == 0) {
        return input_pos;
    }

    // Create a temporary deinterleaver to get the permutation
    FreqDeinterleaverConfig config;
    config.fft_size = fft_size;
    config.num_active_carriers = num_carriers;

    FreqDeinterleaver deint(config);

    // The forward permutation is the inverse of what's stored
    // inv_perm[j] = i means input[i] goes to output[j]
    // So we need to find j where inv_perm[j] = input_pos
    const auto& inv_perm = deint.get_permutation();
    for (size_t j = 0; j < inv_perm.size(); ++j) {
        if (inv_perm[j] == input_pos) {
            return j;
        }
    }

    return input_pos;
}

}  // namespace ofdm
}  // namespace atsc3
