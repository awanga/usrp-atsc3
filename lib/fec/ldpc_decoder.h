#pragma once

// LDPC Decoder — ATSC 3.0 Low-Density Parity-Check decoding
//
// Implements min-sum belief propagation for ATSC 3.0 LDPC codes.
// Supports all 12 code rates (2/15 through 13/15) for both short
// (16200 bit) and long (64800 bit) codewords per ATSC A/322 Section 9.
//
// Uses sparse parity check matrix representation to avoid OOM for
// large codewords. Matrices are loaded from JSON configuration files.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8_t (soft LLRs, codeword_length samples)
//   Output: TDATA=uint8_t (decoded information bits)
//           TVALID TREADY TLAST(codeword boundary)
//
// Reference: ATSC A/322 Section 9 (LDPC Coding)

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atsc3 {
namespace fec {

// LDPC decoder configuration
struct LdpcConfig {
    // Code rate (determines parity check matrix)
    config::CodeRate code_rate = config::CodeRate::RATE_7_15;

    // Codeword length: false = 64800 (long), true = 16200 (short)
    bool short_codeword = false;

    // Maximum decoding iterations
    size_t max_iterations = 50;

    // Early termination threshold (0 = disabled)
    // Stop if average LLR magnitude exceeds this value
    float early_term_threshold = 0.0f;

    // Min-sum scaling factor (typically 0.75-0.9 for better performance)
    float min_sum_scale = 0.75f;
};

// Sparse parity check matrix representation
// Stores only non-zero entries for memory efficiency
struct SparseMatrix {
    // Matrix dimensions
    size_t num_rows = 0;  // Number of parity checks (parity bits)
    size_t num_cols = 0;  // Number of codeword bits

    // Row-wise storage: column indices of non-zero entries per row
    // row_indices[row] = {col1, col2, col3, ...}
    std::vector<std::vector<uint16_t>> row_indices;

    // Column-wise storage: row indices of non-zero entries per column
    // col_indices[col] = {row1, row2, row3, ...}
    std::vector<std::vector<uint16_t>> col_indices;

    // Bidirectional edge index mappings (pre-computed for O(1) lookups)
    // row_to_col_edge[row][i] = edge index in col_indices[row_indices[row][i]]
    // col_to_row_edge[col][i] = edge index in row_indices[col_indices[col][i]]
    std::vector<std::vector<uint16_t>> row_to_col_edge;
    std::vector<std::vector<uint16_t>> col_to_row_edge;

    // Number of information bits (codeword - parity)
    size_t info_bits = 0;

    // Check if matrix is valid
    bool is_valid() const {
        return num_rows > 0 && num_cols > 0 && row_indices.size() == num_rows &&
               col_indices.size() == num_cols && info_bits <= num_cols;
    }

    // Check if edge mappings are built
    bool has_edge_mappings() const {
        return !row_to_col_edge.empty() && !col_to_row_edge.empty();
    }

    // Get degree of a check node (row)
    size_t check_degree(size_t row) const {
        return row < row_indices.size() ? row_indices[row].size() : 0;
    }

    // Get degree of a variable node (column)
    size_t variable_degree(size_t col) const {
        return col < col_indices.size() ? col_indices[col].size() : 0;
    }

    // Build edge mappings (call after row_indices/col_indices are populated)
    void build_edge_mappings();
};

// Decoding result
struct LdpcResult {
    // Decoded information bits (systematic bits only)
    std::vector<uint8_t> decoded_bits;

    // Number of iterations used
    size_t iterations_used = 0;

    // Whether all parity checks are satisfied
    bool converged = false;

    // Number of unsatisfied parity checks (0 if converged)
    size_t num_unsatisfied_checks = 0;

    // Valid flag
    bool is_valid = false;
};

// LDPC Decoder
//
// Implements min-sum belief propagation algorithm for ATSC 3.0 LDPC codes.
// Pre-allocates all working buffers at construction for RTL compatibility.
//
// Usage:
//   LdpcConfig config;
//   config.code_rate = CodeRate::RATE_7_15;
//   config.max_iterations = 50;
//   LdpcDecoder decoder(config);
//
//   // Decode received LLRs
//   LdpcResult result = decoder.decode(llr_input);
//   if (result.converged) {
//       // Use result.decoded_bits
//   }
//
class LdpcDecoder {
public:
    explicit LdpcDecoder(const LdpcConfig& config = LdpcConfig());
    ~LdpcDecoder();

    // Decode one codeword
    // llr_in: Soft LLRs for full codeword (must have codeword_length() elements)
    //         Positive LLR = bit 0 more likely
    // Returns: Decoded information bits and status
    LdpcResult decode(const int8_t* llr_in);

    // Decode with pre-allocated output buffer
    // bits_out: Output buffer for decoded bits (must have info_bits() capacity)
    // Returns: Number of unsatisfied checks (0 = success), or -1 on error
    int decode(const int8_t* llr_in, uint8_t* bits_out, size_t* iterations_out = nullptr);

    // Get codeword length for current configuration
    size_t codeword_length() const {
        return config_.short_codeword ? 16200 : 64800;
    }

    // Get number of information bits
    size_t info_bits() const {
        return H_.info_bits;
    }

    // Get number of parity bits
    size_t parity_bits() const {
        return H_.num_rows;
    }

    // Update configuration (reloads parity matrix)
    void set_config(const LdpcConfig& config);
    const LdpcConfig& get_config() const {
        return config_;
    }

    // Get the parity check matrix (for testing/debugging)
    const SparseMatrix& get_parity_matrix() const {
        return H_;
    }

    // Check if decoder is properly initialized
    bool is_initialized() const {
        return H_.is_valid();
    }

    // Reset internal state
    void reset();

private:
    LdpcConfig config_;
    SparseMatrix H_;

    // Working buffers for min-sum algorithm
    // These are pre-allocated to avoid dynamic allocation during decode

    // Check-to-variable messages: llr_cn_[row][idx] for each edge
    std::vector<std::vector<float>> llr_cn_;

    // Variable-to-check messages: llr_vn_[col][idx] for each edge
    std::vector<std::vector<float>> llr_vn_;

    // A posteriori LLRs for each variable node
    std::vector<float> llr_app_;

    // Channel LLRs (input)
    std::vector<float> llr_channel_;

    // Hard decision bits
    std::vector<uint8_t> hard_decision_;

    // Syndrome check result
    std::vector<uint8_t> syndrome_;

    // Initialize/allocate working buffers
    void allocate_buffers();

    // Load parity check matrix for given code rate
    void load_parity_matrix();

    // Min-sum belief propagation iteration
    void min_sum_iteration();

    // Compute hard decision from current LLRs
    void compute_hard_decision();

    // Check if all parity checks are satisfied
    // Returns number of unsatisfied checks
    size_t check_syndrome();

    // Generate simplified parity matrix for testing
    // (Used when JSON files not available)
    void generate_test_matrix();
};

// Get code rate as a float value (e.g., 7/15 = 0.4667)
inline float code_rate_value(config::CodeRate rate) {
    return config::Atsc3Config::code_rate_value(rate);
}

// Get number of information bits for given codeword length and code rate
size_t get_ldpc_info_bits(bool short_codeword, config::CodeRate rate);

// Get number of parity bits for given codeword length and code rate
size_t get_ldpc_parity_bits(bool short_codeword, config::CodeRate rate);

}  // namespace fec
}  // namespace atsc3
