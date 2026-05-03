#pragma once

// Time De-interleaver — ATSC 3.0 convolutional time de-interleaving
//
// Reverses the convolutional time interleaving applied at the transmitter
// to improve burst error resilience across time. Uses multiple delay lines
// with progressively longer delays.
//
// Supports two modes per ATSC A/322:
//   CTI: Convolutional Time Interleaving
//   HTI: Hybrid Time Interleaving (not yet implemented)
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8_t (interleaved LLR cells)
//   Output: TDATA=int8_t (de-interleaved LLR cells)
//           TVALID TREADY TLAST(TI block boundary)
//
// Reference: ATSC A/322 Section 8.2 (Time Interleaving)

#include "config/atsc3_config.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Time de-interleaver configuration
struct TimeDeinterleaverConfig {
    // Time interleaving mode
    config::TimeInterleaveMode mode = config::TimeInterleaveMode::CTI;

    // TI depth (0-15) - determines delay line lengths
    // depth=0 means no time interleaving
    uint8_t depth = 0;

    // Number of TI blocks (rows in interleaver matrix)
    size_t num_ti_blocks = 0;

    // Cells per TI block (columns in interleaver matrix)
    size_t cells_per_block = 0;
};

// Time De-interleaver
//
// Implements convolutional de-interleaving using delay lines.
// For CTI mode with depth D:
//   - N_rows = (D + 1) delay lines
//   - Row i has delay = (N_rows - 1 - i) * cells_per_block
//
// The de-interleaver has the inverse delay pattern of the interleaver.
//
// Usage:
//   TimeDeinterleaverConfig config;
//   config.mode = TimeInterleaveMode::CTI;
//   config.depth = 4;
//   config.cells_per_block = 1000;
//   TimeDeinterleaver deint(config);
//
//   // Process TI blocks sequentially
//   deint.process(input_block, output_block, cells_per_block);
//
class TimeDeinterleaver {
public:
    explicit TimeDeinterleaver(const TimeDeinterleaverConfig& config = TimeDeinterleaverConfig());
    ~TimeDeinterleaver();

    // Process one TI block
    // input: Interleaved cells for this block
    // output: De-interleaved cells (may contain cells from previous blocks)
    // num_cells: Number of cells in this block
    //
    // Note: Initial blocks will contain invalid data as delay lines fill
    void process(const int8_t* input, int8_t* output, size_t num_cells);

    // Get number of blocks needed to fill delay lines
    // Output is invalid for the first get_settling_blocks() blocks
    size_t get_settling_blocks() const {
        return settling_blocks_;
    }

    // Check if delay lines are filled (output is valid)
    // Output becomes valid after settling_blocks_ blocks have been processed
    // For depth=0 (no interleaving), output is immediately valid
    bool is_settled() const {
        if (settling_blocks_ == 0) {
            return true;
        }
        return blocks_processed_ > settling_blocks_;
    }

    // Get total blocks processed
    size_t blocks_processed() const {
        return blocks_processed_;
    }

    // Get current configuration
    const TimeDeinterleaverConfig& get_config() const {
        return config_;
    }

    // Update configuration (resets state)
    void set_config(const TimeDeinterleaverConfig& config);

    // Reset internal state (clears delay lines)
    void reset();

private:
    TimeDeinterleaverConfig config_;

    // Number of rows (delay lines)
    size_t num_rows_;

    // Delay lines: delay_lines_[row][position]
    // Each row has a different length based on its delay
    std::vector<std::vector<int8_t>> delay_lines_;

    // Write position for each delay line (circular buffer)
    std::vector<size_t> write_pos_;

    // Number of blocks needed for settling
    size_t settling_blocks_;

    // Blocks processed so far
    size_t blocks_processed_;

    // Initialize delay lines based on config
    void init_delay_lines();

    // Process a single cell through the appropriate delay line
    int8_t process_cell(int8_t input_cell, size_t cell_index);
};

}  // namespace ofdm
}  // namespace atsc3
