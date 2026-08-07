#pragma once

// LDPC Matrix Generator — ATSC 3.0 quasi-cyclic LDPC parity check matrices
//
// Generates H matrices per ATSC A/322 Section 9.2 for all code rates and
// codeword lengths. Uses quasi-cyclic construction with circulant permutation
// matrices.
//
// Structure:
//   - Expansion factor Q = 360 (long codes, N=64800) or Q = 90 (short codes, N=16200)
//   - Base matrix dimensions vary by code rate
//   - Each base matrix entry is either -1 (zero QxQ block) or 0 to Q-1 (circulant shift)
//
// Reference: ATSC A/322 Section 9 (LDPC Coding), Tables 9.1-9.24

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atsc3 {
namespace fec {

// Forward declaration
struct SparseMatrix;

// Base matrix entry type (-1 = zero block, 0 to Q-1 = circulant shift)
using BaseMatrixEntry = int16_t;

// Base matrix for quasi-cyclic LDPC construction
struct LdpcBaseMatrix {
    // Number of block rows (parity_bits / Q)
    size_t num_block_rows = 0;

    // Number of block columns (codeword_length / Q)
    size_t num_block_cols = 0;

    // Expansion factor Q
    size_t expansion_factor = 0;

    // Base matrix entries (num_block_rows x num_block_cols)
    // Entry -1 means zero Q×Q block
    // Entry 0 to Q-1 means circulant permutation matrix with that shift
    std::vector<std::vector<BaseMatrixEntry>> entries;

    // Check if valid
    bool is_valid() const {
        return num_block_rows > 0 && num_block_cols > 0 && expansion_factor > 0 &&
               entries.size() == num_block_rows;
    }
};

// LDPC matrix generator
//
// Generates sparse H matrices from base matrices per ATSC A/322.
// Supports all 12 code rates for both short (16200) and long (64800) codewords.
//
// Usage:
//   LdpcMatrixGenerator gen;
//   SparseMatrix H = gen.generate(CodeRate::RATE_7_15, false);  // 7/15 long
//
class LdpcMatrixGenerator {
public:
    LdpcMatrixGenerator() = default;

    // Generate sparse H matrix for given code rate and length
    // short_codeword: true = 16200 bits, false = 64800 bits
    SparseMatrix generate(config::CodeRate rate, bool short_codeword) const;

    // Get base matrix for given code rate and length
    LdpcBaseMatrix get_base_matrix(config::CodeRate rate, bool short_codeword) const;

    // Expand base matrix to full sparse matrix
    static SparseMatrix expand_base_matrix(const LdpcBaseMatrix& base, size_t info_bits);

    // Get expansion factor Q for codeword length
    static size_t get_expansion_factor(bool short_codeword) {
        return short_codeword ? 90 : 360;
    }

    // Validate generated matrix against expected dimensions
    bool validate_matrix(const SparseMatrix& H, config::CodeRate rate, bool short_codeword) const;

private:
    // Generate base matrix entries per ATSC A/322
    // These implement the circulant shift patterns from the specification
    LdpcBaseMatrix generate_base_matrix_long(config::CodeRate rate) const;
    LdpcBaseMatrix generate_base_matrix_short(config::CodeRate rate) const;
};

// Load H matrix from JSON file
// Returns empty matrix on failure
SparseMatrix load_ldpc_matrix(const std::string& filepath);

// Save H matrix to JSON file
// Returns true on success
bool save_ldpc_matrix(const SparseMatrix& H, const std::string& filepath);

// Get file path for cached H matrix
std::string get_ldpc_matrix_path(config::CodeRate rate, bool short_codeword);

}  // namespace fec
}  // namespace atsc3
