// ldpc_decoder.cc — ATSC 3.0 LDPC decoding using min-sum algorithm
//
// Implements belief propagation for ATSC 3.0 LDPC codes.
//
// Reference: ATSC A/322 Section 9 (LDPC Coding)

#include "ldpc_decoder.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace atsc3 {
namespace fec {

namespace {

// LLR magnitude limit to prevent overflow
constexpr float LLR_MAX = 127.0f;
constexpr float LLR_MIN = -127.0f;

// Clamp LLR value to valid range
inline float clamp_llr(float llr) {
    return std::max(LLR_MIN, std::min(LLR_MAX, llr));
}

// Sign of a value (returns -1, 0, or 1)
inline float sign(float x) {
    return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
}

// ATSC 3.0 LDPC code parameters
// Information bits = codeword_length * code_rate
// Parity bits = codeword_length - information_bits

struct LdpcCodeParams {
    size_t info_bits_long;   // Info bits for 64800 codeword
    size_t info_bits_short;  // Info bits for 16200 codeword
};

// Code parameters for each rate (info bits)
// Rate k/15 means info_bits = codeword_length * k / 15
const LdpcCodeParams kLdpcParams[] = {
    {8640, 2160},    // 2/15
    {12960, 3240},   // 3/15
    {17280, 4320},   // 4/15
    {21600, 5400},   // 5/15
    {25920, 6480},   // 6/15
    {30240, 7560},   // 7/15
    {34560, 8640},   // 8/15
    {38880, 9720},   // 9/15
    {43200, 10800},  // 10/15
    {47520, 11880},  // 11/15
    {51840, 12960},  // 12/15
    {56160, 14040},  // 13/15
};

}  // namespace

size_t get_ldpc_info_bits(bool short_codeword, config::CodeRate rate) {
    size_t idx = static_cast<size_t>(rate);
    if (idx >= sizeof(kLdpcParams) / sizeof(kLdpcParams[0])) {
        return 0;
    }
    return short_codeword ? kLdpcParams[idx].info_bits_short : kLdpcParams[idx].info_bits_long;
}

size_t get_ldpc_parity_bits(bool short_codeword, config::CodeRate rate) {
    size_t codeword_len = short_codeword ? 16200 : 64800;
    return codeword_len - get_ldpc_info_bits(short_codeword, rate);
}

LdpcDecoder::LdpcDecoder(const LdpcConfig& config) : config_(config) {
    load_parity_matrix();
    allocate_buffers();
}

LdpcDecoder::~LdpcDecoder() = default;

void LdpcDecoder::load_parity_matrix() {
    // Get code dimensions
    size_t codeword_len = codeword_length();
    size_t info = get_ldpc_info_bits(config_.short_codeword, config_.code_rate);
    size_t parity = codeword_len - info;

    H_.num_cols = codeword_len;
    H_.num_rows = parity;
    H_.info_bits = info;

    // TODO: Load actual ATSC 3.0 parity matrices from JSON files
    // For now, generate a simplified test matrix
    generate_test_matrix();
}

void LdpcDecoder::generate_test_matrix() {
    // Generate a simple regular LDPC matrix for testing
    // This is NOT the actual ATSC 3.0 matrix, but allows testing the algorithm
    //
    // Structure: quasi-cyclic with block size Q
    // Each block row has constant number of 1s per row
    // This provides reasonable error correction for testing

    size_t n = H_.num_cols;  // Codeword length
    size_t m = H_.num_rows;  // Parity checks

    // Use smaller block size for manageability
    // Real ATSC 3.0 uses Q=360 for long codes, Q=90 for short codes
    size_t Q = config_.short_codeword ? 90 : 360;

    // Number of blocks
    size_t n_blocks = n / Q;
    size_t m_blocks = m / Q;

    // Row weight (edges per check node)
    // Typical LDPC: row_weight ~ 6-20
    size_t row_weight = 6;  // Simplified

    // Initialize storage
    H_.row_indices.resize(m);
    H_.col_indices.resize(n);

    // Generate quasi-cyclic structure
    // For each block position, add a circulant permutation matrix
    for (size_t mb = 0; mb < m_blocks; ++mb) {
        // Determine which column blocks this row block connects to
        // Use a simple pattern: connect to row_weight consecutive blocks
        for (size_t w = 0; w < row_weight && (mb + w) < n_blocks; ++w) {
            size_t nb = (mb + w) % n_blocks;

            // Add circulant shift (different for each block)
            size_t shift = (mb * row_weight + w) % Q;

            // Add connections for this Q x Q block
            for (size_t i = 0; i < Q; ++i) {
                size_t row = mb * Q + i;
                size_t col = nb * Q + ((i + shift) % Q);

                if (row < m && col < n) {
                    H_.row_indices[row].push_back(static_cast<uint16_t>(col));
                    H_.col_indices[col].push_back(static_cast<uint16_t>(row));
                }
            }
        }
    }

    // Sort indices for consistent ordering
    for (auto& row : H_.row_indices) {
        std::sort(row.begin(), row.end());
    }
    for (auto& col : H_.col_indices) {
        std::sort(col.begin(), col.end());
    }
}

void LdpcDecoder::allocate_buffers() {
    if (!H_.is_valid()) {
        return;
    }

    size_t n = H_.num_cols;
    size_t m = H_.num_rows;

    // Allocate check-to-variable messages (one per edge, grouped by row)
    llr_cn_.resize(m);
    for (size_t row = 0; row < m; ++row) {
        llr_cn_[row].resize(H_.row_indices[row].size(), 0.0f);
    }

    // Allocate variable-to-check messages (one per edge, grouped by column)
    llr_vn_.resize(n);
    for (size_t col = 0; col < n; ++col) {
        llr_vn_[col].resize(H_.col_indices[col].size(), 0.0f);
    }

    // Allocate other buffers
    llr_app_.resize(n, 0.0f);
    llr_channel_.resize(n, 0.0f);
    hard_decision_.resize(n, 0);
    syndrome_.resize(m, 0);
}

void LdpcDecoder::set_config(const LdpcConfig& config) {
    config_ = config;
    load_parity_matrix();
    allocate_buffers();
}

void LdpcDecoder::reset() {
    // Clear working buffers
    for (auto& row : llr_cn_) {
        std::fill(row.begin(), row.end(), 0.0f);
    }
    for (auto& col : llr_vn_) {
        std::fill(col.begin(), col.end(), 0.0f);
    }
    std::fill(llr_app_.begin(), llr_app_.end(), 0.0f);
    std::fill(llr_channel_.begin(), llr_channel_.end(), 0.0f);
    std::fill(hard_decision_.begin(), hard_decision_.end(), 0);
    std::fill(syndrome_.begin(), syndrome_.end(), 0);
}

LdpcResult LdpcDecoder::decode(const int8_t* llr_in) {
    LdpcResult result;

    if (llr_in == nullptr || !H_.is_valid()) {
        return result;
    }

    size_t n = H_.num_cols;

    // Initialize channel LLRs from input
    for (size_t i = 0; i < n; ++i) {
        llr_channel_[i] = static_cast<float>(llr_in[i]);
    }

    // Initialize variable-to-check messages with channel LLRs
    for (size_t col = 0; col < n; ++col) {
        for (size_t idx = 0; idx < llr_vn_[col].size(); ++idx) {
            llr_vn_[col][idx] = llr_channel_[col];
        }
    }

    // Clear check-to-variable messages
    for (auto& row : llr_cn_) {
        std::fill(row.begin(), row.end(), 0.0f);
    }

    // Iterative decoding
    size_t iter;
    for (iter = 0; iter < config_.max_iterations; ++iter) {
        // Min-sum iteration
        min_sum_iteration();

        // Compute hard decision
        compute_hard_decision();

        // Check syndrome
        size_t unsatisfied = check_syndrome();
        if (unsatisfied == 0) {
            // All parity checks satisfied
            result.converged = true;
            result.iterations_used = iter + 1;
            break;
        }

        // Early termination check based on LLR magnitude
        if (config_.early_term_threshold > 0.0f) {
            float avg_magnitude = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                avg_magnitude += std::abs(llr_app_[i]);
            }
            avg_magnitude /= static_cast<float>(n);

            if (avg_magnitude > config_.early_term_threshold) {
                // LLRs are confident but parity failed - likely uncorrectable
                break;
            }
        }
    }

    // Fill result
    result.iterations_used = iter + 1;
    result.num_unsatisfied_checks = check_syndrome();
    result.converged = (result.num_unsatisfied_checks == 0);

    // Extract information bits (systematic code: first K bits)
    result.decoded_bits.resize(H_.info_bits);
    for (size_t i = 0; i < H_.info_bits; ++i) {
        result.decoded_bits[i] = hard_decision_[i];
    }

    result.is_valid = true;
    return result;
}

int LdpcDecoder::decode(const int8_t* llr_in, uint8_t* bits_out, size_t* iterations_out) {
    LdpcResult result = decode(llr_in);

    if (!result.is_valid) {
        return -1;
    }

    if (bits_out != nullptr) {
        std::copy(result.decoded_bits.begin(), result.decoded_bits.end(), bits_out);
    }

    if (iterations_out != nullptr) {
        *iterations_out = result.iterations_used;
    }

    return static_cast<int>(result.num_unsatisfied_checks);
}

void LdpcDecoder::min_sum_iteration() {
    size_t m = H_.num_rows;
    size_t n = H_.num_cols;
    float scale = config_.min_sum_scale;

    // Check node update (horizontal step)
    // For each check node, compute messages to connected variable nodes
    for (size_t row = 0; row < m; ++row) {
        const auto& cols = H_.row_indices[row];
        size_t degree = cols.size();

        if (degree == 0)
            continue;

        // Compute product of signs and find two smallest magnitudes
        float sign_prod = 1.0f;
        float min1 = std::numeric_limits<float>::max();  // Smallest
        float min2 = std::numeric_limits<float>::max();  // Second smallest
        size_t min1_idx = 0;

        for (size_t idx = 0; idx < degree; ++idx) {
            size_t col = cols[idx];

            // Find which edge index this is in the column's perspective
            const auto& rows_for_col = H_.col_indices[col];
            size_t edge_idx = 0;
            for (size_t j = 0; j < rows_for_col.size(); ++j) {
                if (rows_for_col[j] == row) {
                    edge_idx = j;
                    break;
                }
            }

            float vn_msg = llr_vn_[col][edge_idx];
            float mag = std::abs(vn_msg);

            sign_prod *= sign(vn_msg);

            if (mag < min1) {
                min2 = min1;
                min1 = mag;
                min1_idx = idx;
            } else if (mag < min2) {
                min2 = mag;
            }
        }

        // Compute check-to-variable messages
        for (size_t idx = 0; idx < degree; ++idx) {
            // Min-sum approximation: message excludes this variable
            float this_sign = sign(llr_vn_[cols[idx]][0]);  // Simplified
            float msg_sign = sign_prod * this_sign;

            // Use min2 for the minimum variable, min1 for others
            float msg_mag = (idx == min1_idx) ? min2 : min1;

            // Apply scaling factor for better performance
            llr_cn_[row][idx] = clamp_llr(msg_sign * msg_mag * scale);
        }
    }

    // Variable node update (vertical step)
    // For each variable node, compute messages to connected check nodes
    for (size_t col = 0; col < n; ++col) {
        const auto& rows = H_.col_indices[col];
        size_t degree = rows.size();

        if (degree == 0) {
            llr_app_[col] = llr_channel_[col];
            continue;
        }

        // Sum all incoming check-to-variable messages
        float sum_cn = 0.0f;
        for (size_t idx = 0; idx < degree; ++idx) {
            size_t row = rows[idx];

            // Find which edge index this is in the row's perspective
            const auto& cols_for_row = H_.row_indices[row];
            size_t edge_idx = 0;
            for (size_t j = 0; j < cols_for_row.size(); ++j) {
                if (cols_for_row[j] == col) {
                    edge_idx = j;
                    break;
                }
            }

            sum_cn += llr_cn_[row][edge_idx];
        }

        // A posteriori LLR
        llr_app_[col] = clamp_llr(llr_channel_[col] + sum_cn);

        // Compute variable-to-check messages (exclude incoming message from each check)
        for (size_t idx = 0; idx < degree; ++idx) {
            size_t row = rows[idx];

            // Find edge index in row
            const auto& cols_for_row = H_.row_indices[row];
            size_t edge_idx = 0;
            for (size_t j = 0; j < cols_for_row.size(); ++j) {
                if (cols_for_row[j] == col) {
                    edge_idx = j;
                    break;
                }
            }

            float cn_msg = llr_cn_[row][edge_idx];
            llr_vn_[col][idx] = clamp_llr(llr_app_[col] - cn_msg);
        }
    }
}

void LdpcDecoder::compute_hard_decision() {
    size_t n = H_.num_cols;

    for (size_t i = 0; i < n; ++i) {
        // Negative LLR -> bit = 1, Positive LLR -> bit = 0
        hard_decision_[i] = (llr_app_[i] < 0.0f) ? 1 : 0;
    }
}

size_t LdpcDecoder::check_syndrome() {
    size_t m = H_.num_rows;
    size_t unsatisfied = 0;

    for (size_t row = 0; row < m; ++row) {
        const auto& cols = H_.row_indices[row];

        // XOR all bits in this check
        uint8_t parity = 0;
        for (uint16_t col : cols) {
            parity ^= hard_decision_[col];
        }

        syndrome_[row] = parity;
        if (parity != 0) {
            ++unsatisfied;
        }
    }

    return unsatisfied;
}

}  // namespace fec
}  // namespace atsc3
