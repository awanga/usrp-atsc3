#pragma once

// Cell De-interleaver — ATSC 3.0 cell-level de-interleaving
//
// Reverses the cell interleaving applied at the transmitter to improve
// burst error resilience. Uses a pseudo-random permutation based on
// cell count.
//
// The interleaver permutation is defined by ATSC A/322 Section 8.1.
// De-interleaving applies the inverse permutation.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8_t (interleaved LLR cells)
//   Output: TDATA=int8_t (de-interleaved LLR cells)
//           TVALID TREADY TLAST(FEC block boundary)
//
// Reference: ATSC A/322 Section 8.1 (Cell Interleaving)

#include "simd/simd_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Cell de-interleaver configuration
struct CellDeinterleaverConfig {
    // Number of cells per interleaving block
    // Determined by FEC block size and modulation
    size_t num_cells = 0;
};

// Cell De-interleaver
//
// Applies inverse permutation to recover original cell ordering.
// Permutation is pre-computed at construction for efficiency.
//
// Usage:
//   CellDeinterleaverConfig config;
//   config.num_cells = 10800;  // Example: 64800 bits / 6 bits per QAM64 symbol
//   CellDeinterleaver deint(config);
//
//   // De-interleave in-place
//   deint.deinterleave(cells, num_cells);
//
class CellDeinterleaver {
public:
    explicit CellDeinterleaver(const CellDeinterleaverConfig& config = CellDeinterleaverConfig());
    ~CellDeinterleaver();

    // De-interleave cell array in-place
    // cells: Array of LLR values (num_cells elements)
    // num_cells: Number of cells to de-interleave
    void deinterleave(int8_t* cells, size_t num_cells);

    // De-interleave from input to output buffer
    // input: Interleaved cells
    // output: De-interleaved cells (must be different from input)
    // num_cells: Number of cells
    void deinterleave(const int8_t* input, int8_t* output, size_t num_cells) const;

    // Get current configuration
    const CellDeinterleaverConfig& get_config() const {
        return config_;
    }

    // Update configuration (recomputes permutation)
    void set_config(const CellDeinterleaverConfig& config);

    // Get the inverse permutation table
    const simd::aligned_vector<size_t>& get_permutation() const {
        return inv_permutation_;
    }

    // Reset internal state
    void reset();

private:
    CellDeinterleaverConfig config_;

    // Inverse permutation: inv_permutation_[i] = source index for output position i
    // Cache-line aligned for optimal memory access
    simd::aligned_vector<size_t> inv_permutation_;

    // Temporary buffer for in-place de-interleaving
    // Cache-line aligned for SIMD operations
    simd::aligned_vector<int8_t> temp_buffer_;

    // Compute the inverse permutation for given cell count
    void compute_permutation(size_t num_cells);

    // ATSC A/322 cell interleaver permutation generator
    // Returns the interleaved position for a given input position
    size_t interleaver_permutation(size_t input_pos, size_t num_cells) const;
};

// Compute the cell interleaver permutation for testing
// Returns the output position for a given input position
size_t compute_cell_interleaver_position(size_t input_pos, size_t num_cells);

}  // namespace ofdm
}  // namespace atsc3
