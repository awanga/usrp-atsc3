// cell_deinterleaver.cc — ATSC 3.0 cell-level de-interleaving
//
// Reverses cell interleaving to recover original ordering.
// Includes SIMD-optimized paths for SSSE3 and AVX2 tiers.
//
// Reference: ATSC A/322 Section 8.1 (Cell Interleaving)

#include "cell_deinterleaver.h"
#include "simd/cpu_features.h"
#include "simd/simd_types.h"

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

// Count bits in a number
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

// SIMD-optimized gather for int8_t data
// Gathers elements from src using 32-bit indices
// For irregular permutations, scalar gather + vector store is often best

#if defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

// AVX2 optimized gather using vgatherdd for indices
// Note: For int8_t, we gather 4 bytes at a time and extract the needed byte
inline void gather_i8_avx2(const int8_t* src, int8_t* dst, const size_t* indices, size_t n) {
    using namespace atsc3::simd;

    // Process 32 elements at a time (best for cache efficiency)
    const size_t simd_width = 32;
    size_t i = 0;

    // Main SIMD loop
    for (; i + simd_width <= n; i += simd_width) {
        // Prefetch next batch of indices and source data
        simd_prefetch_read(&indices[i + simd_width]);

        // Scalar gather into aligned buffer (AVX2 gather is for 32-bit, not 8-bit)
        ATSC3_ALIGN_32 int8_t temp[32];
        for (size_t j = 0; j < simd_width; ++j) {
            temp[j] = src[indices[i + j]];
        }

        // Store gathered data
        simd_i8x32 v = simd_load_i8x32(temp);
        simd_storeu_i8x32(&dst[i], v);
    }

    // Scalar cleanup
    for (; i < n; ++i) {
        dst[i] = src[indices[i]];
    }
}

#endif  // ATSC3_SIMD_AVX2

#if defined(ATSC3_SIMD_SSSE3) || defined(ATSC3_SIMD_AVX2) || defined(ATSC3_SIMD_NATIVE)

// SSSE3 optimized gather (128-bit)
inline void gather_i8_ssse3(const int8_t* src, int8_t* dst, const size_t* indices, size_t n) {
    using namespace atsc3::simd;

    // Process 16 elements at a time
    const size_t simd_width = 16;
    size_t i = 0;

    // Main SIMD loop
    for (; i + simd_width <= n; i += simd_width) {
        // Prefetch next batch
        simd_prefetch_read(&indices[i + simd_width]);

        // Scalar gather into aligned buffer
        ATSC3_ALIGN_16 int8_t temp[16];
        for (size_t j = 0; j < simd_width; ++j) {
            temp[j] = src[indices[i + j]];
        }

        // Store gathered data
        simd_i8x16 v = simd_load_i8x16(temp);
        simd_storeu_i8x16(&dst[i], v);
    }

    // Scalar cleanup
    for (; i < n; ++i) {
        dst[i] = src[indices[i]];
    }
}

#endif  // ATSC3_SIMD_SSSE3

// Scalar gather (fallback)
inline void gather_i8_scalar(const int8_t* src, int8_t* dst, const size_t* indices, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[indices[i]];
    }
}

// Runtime dispatch based on CPU features
inline void gather_i8_dispatch(const int8_t* src, int8_t* dst, const size_t* indices, size_t n) {
#if defined(ATSC3_SIMD_AVX2)
    gather_i8_avx2(src, dst, indices, n);
#elif defined(ATSC3_SIMD_SSSE3)
    gather_i8_ssse3(src, dst, indices, n);
#elif defined(ATSC3_SIMD_NATIVE)
    // Runtime dispatch for NATIVE builds
    using namespace atsc3::simd;
    if (CpuFeatures::has_avx2_support()) {
        gather_i8_avx2(src, dst, indices, n);
    } else if (CpuFeatures::has_ssse3_support()) {
        gather_i8_ssse3(src, dst, indices, n);
    } else {
        gather_i8_scalar(src, dst, indices, n);
    }
#else
    gather_i8_scalar(src, dst, indices, n);
#endif
}

}  // namespace

CellDeinterleaver::CellDeinterleaver(const CellDeinterleaverConfig& config) : config_(config) {
    if (config_.num_cells > 0) {
        compute_permutation(config_.num_cells);
    }
}

CellDeinterleaver::~CellDeinterleaver() = default;

void CellDeinterleaver::compute_permutation(size_t num_cells) {
    if (num_cells == 0) {
        inv_permutation_.clear();
        temp_buffer_.clear();
        return;
    }

    // ATSC A/322 cell interleaver uses a pseudo-random permutation
    // based on bit-reversal with pruning for non-power-of-2 sizes
    //
    // Bit-reversal interleaver: input[i] → output[bit_reverse(i)]
    // For non-power-of-2, only positions where both i and bit_reverse(i) < num_cells are valid
    //
    // For de-interleaving we compute the mathematical inverse of the forward permutation

    size_t n_padded = next_power_of_two(num_cells);
    size_t num_bits = count_bits(n_padded - 1);
    if (num_bits == 0)
        num_bits = 1;

    // For power-of-2 sizes, bit-reversal is its own inverse
    // For non-power-of-2, we need to build a proper bijection

    std::vector<size_t> fwd_perm(num_cells);

    if (num_cells == n_padded) {
        // Power-of-2: simple bit reversal
        for (size_t i = 0; i < num_cells; ++i) {
            fwd_perm[i] = bit_reverse(i, num_bits);
        }
    } else {
        // Non-power-of-2: build permutation by collecting valid mappings
        // Use a pruned bit-reversal: skip positions that map out of range
        // and compress the remaining positions

        // First, identify which positions have valid bit-reversal mappings
        std::vector<std::pair<size_t, size_t>> valid_pairs;
        for (size_t i = 0; i < n_padded; ++i) {
            size_t rev = bit_reverse(i, num_bits);
            if (i < num_cells && rev < num_cells) {
                valid_pairs.push_back({i, rev});
            }
        }

        // Build mapping using valid pairs
        std::vector<bool> src_used(num_cells, false);
        std::vector<bool> dst_used(num_cells, false);

        for (const auto& [src, dst] : valid_pairs) {
            if (!src_used[src] && !dst_used[dst]) {
                fwd_perm[src] = dst;
                src_used[src] = true;
                dst_used[dst] = true;
            }
        }

        // Fill remaining with identity (for positions not covered)
        std::vector<size_t> unused_dst;
        for (size_t i = 0; i < num_cells; ++i) {
            if (!dst_used[i]) {
                unused_dst.push_back(i);
            }
        }

        size_t unused_idx = 0;
        for (size_t i = 0; i < num_cells; ++i) {
            if (!src_used[i]) {
                fwd_perm[i] = unused_dst[unused_idx++];
            }
        }
    }

    // Compute mathematical inverse for de-interleaving
    // If fwd_perm[i] = j, then inv_permutation_[j] = i
    // De-interleaver: output[i] = input[inv_permutation_[i]]
    inv_permutation_.resize(num_cells);
    for (size_t i = 0; i < num_cells; ++i) {
        inv_permutation_[fwd_perm[i]] = i;
    }

    // Pre-allocate temp buffer for in-place operation
    temp_buffer_.resize(num_cells);
}

size_t CellDeinterleaver::interleaver_permutation(size_t input_pos, size_t num_cells) const {
    if (num_cells == 0 || input_pos >= num_cells) {
        return input_pos;
    }

    size_t n_padded = next_power_of_two(num_cells);
    size_t num_bits = count_bits(n_padded - 1);
    if (num_bits == 0)
        num_bits = 1;

    // Find the output position by iterating through bit-reversed sequence
    size_t count = 0;
    for (size_t i = 0; i < n_padded; ++i) {
        size_t reversed = bit_reverse(i, num_bits);
        if (reversed < num_cells) {
            if (count == input_pos) {
                return reversed;
            }
            ++count;
        }
    }

    return input_pos;
}

void CellDeinterleaver::deinterleave(int8_t* cells, size_t num_cells) {
    if (cells == nullptr || num_cells == 0) {
        return;
    }

    // Recompute permutation if size changed
    if (num_cells != inv_permutation_.size()) {
        compute_permutation(num_cells);
    }

    // Copy to temp buffer
    std::copy(cells, cells + num_cells, temp_buffer_.begin());

    // Apply inverse permutation using SIMD-optimized gather
    // inv_permutation_[i] tells us where to get the value for output position i
    gather_i8_dispatch(temp_buffer_.data(), cells, inv_permutation_.data(), num_cells);
}

void CellDeinterleaver::deinterleave(const int8_t* input, int8_t* output, size_t num_cells) const {
    if (input == nullptr || output == nullptr || num_cells == 0) {
        return;
    }

    // If permutation size doesn't match, use identity
    if (num_cells != inv_permutation_.size()) {
        std::copy(input, input + num_cells, output);
        return;
    }

    // Apply inverse permutation using SIMD-optimized gather
    gather_i8_dispatch(input, output, inv_permutation_.data(), num_cells);
}

void CellDeinterleaver::set_config(const CellDeinterleaverConfig& config) {
    config_ = config;
    if (config_.num_cells > 0) {
        compute_permutation(config_.num_cells);
    } else {
        inv_permutation_.clear();
        temp_buffer_.clear();
    }
}

void CellDeinterleaver::reset() {
    // No state to reset beyond permutation tables
}

size_t compute_cell_interleaver_position(size_t input_pos, size_t num_cells) {
    if (num_cells == 0 || input_pos >= num_cells) {
        return input_pos;
    }

    size_t n_padded = next_power_of_two(num_cells);
    size_t num_bits = count_bits(n_padded - 1);
    if (num_bits == 0)
        num_bits = 1;

    size_t count = 0;
    for (size_t i = 0; i < n_padded; ++i) {
        size_t reversed = bit_reverse(i, num_bits);
        if (reversed < num_cells) {
            if (count == input_pos) {
                return reversed;
            }
            ++count;
        }
    }

    return input_pos;
}

}  // namespace ofdm
}  // namespace atsc3
