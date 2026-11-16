#pragma once

// LDPC Matrix Generator — ATSC 3.0 parity check matrices and encoder
//
// Builds the parity check matrix H of each A/322 LDPC code (rates 2/15 to
// 13/15, Ninner = 64800 and 16200) from the Annex A accumulator tables
// (lib/fec/ldpc_tables.h), following the encoding rules of A/322 Section
// 6.1.3:
//   Type A: info bits accumulate into an M1 dual-diagonal part (step Q1)
//           and an M2 identity part (step Q2); the M1 parity bits feed the
//           M2 part too. Both parity parts are column-interleaved in the
//           codeword (steps vi and viii).
//   Type B: info bits accumulate with step Qldpc over a single
//           dual-diagonal parity part.
// Columns follow the codeword order Λ that the encoder emits, i.e. before
// the bit interleaver (Section 6.2), whose parity interleaver the receiver
// undoes upstream.
//
// Reference: ATSC A/322 Section 6.1.3, Tables 6.4-6.7, Annex A

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atsc3 {
namespace fec {

// Forward declaration
struct SparseMatrix;

// LDPC matrix generator
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
    // Returns an empty (invalid) matrix for a rate outside the enum.
    SparseMatrix generate(config::CodeRate rate, bool short_codeword) const;

    // Bits per accumulator group (A/322 uses 360 for both code lengths)
    static size_t get_expansion_factor(bool short_codeword) {
        (void)short_codeword;
        return 360;
    }

    // Validate generated matrix against expected dimensions
    bool validate_matrix(const SparseMatrix& H, config::CodeRate rate, bool short_codeword) const;
};

// Systematic encoder of A/322 Section 6.1.3 (the transmitter's procedure).
// info: Nouter bits, one per byte (0/1). codeword: Ninner bytes, written as
// the codeword Λ (info bits then parity bits, before bit interleaving).
// Returns false for a rate outside the enum.
bool ldpc_encode(config::CodeRate rate, bool short_codeword, const uint8_t* info,
                 uint8_t* codeword);

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
