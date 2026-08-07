// time_deinterleaver.cc — ATSC 3.0 convolutional time de-interleaving
//
// Reverses convolutional time interleaving using delay lines.
//
// Reference: ATSC A/322 Section 8.2 (Time Interleaving)

#include "time_deinterleaver.h"

#include <algorithm>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

namespace {

// Check if n is a power of 2
inline bool is_power_of_two(size_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

}  // namespace

TimeDeinterleaver::TimeDeinterleaver(const TimeDeinterleaverConfig& config)
    : config_(config), num_rows_(0), row_mask_(0), settling_blocks_(0), blocks_processed_(0) {
    init_delay_lines();
}

TimeDeinterleaver::~TimeDeinterleaver() = default;

void TimeDeinterleaver::init_delay_lines() {
    // depth = 0 means no time interleaving
    if (config_.depth == 0 || config_.mode == config::TimeInterleaveMode::NONE) {
        num_rows_ = 1;
        row_mask_ = 0;
        settling_blocks_ = 0;
        delay_lines_.clear();
        delay_masks_.clear();
        write_pos_.clear();
        return;
    }

    // For CTI mode:
    // Number of rows = depth + 1
    // Row i has delay = i * cells_per_block for interleaver
    // De-interleaver has inverse delays: row i has delay = (num_rows - 1 - i) * cells_per_block
    num_rows_ = config_.depth + 1;

    // Set up bitmask for row selection if num_rows is power-of-2
    // This enables bit-masking optimization for depths 1, 3, 7, 15 (rows 2, 4, 8, 16)
    if (is_power_of_two(num_rows_)) {
        row_mask_ = num_rows_ - 1;
    } else {
        row_mask_ = 0;  // Fall back to modulo
    }

    delay_lines_.resize(num_rows_);
    delay_masks_.resize(num_rows_, 0);
    write_pos_.resize(num_rows_, 0);

    // Pre-allocate delay lines
    // De-interleaver delay for row i = (num_rows - 1 - i) * cells_per_block
    for (size_t row = 0; row < num_rows_; ++row) {
        size_t delay = (num_rows_ - 1 - row) * config_.cells_per_block;
        if (delay > 0) {
            delay_lines_[row].resize(delay, 0);
            delay_masks_[row] = delay;
        } else {
            delay_lines_[row].clear();
            delay_masks_[row] = 0;
        }
    }

    // Calculate settling time (max delay in blocks)
    settling_blocks_ = num_rows_ - 1;
    blocks_processed_ = 0;
}

void TimeDeinterleaver::process(const int8_t* input, int8_t* output, size_t num_cells) {
    if (input == nullptr || output == nullptr || num_cells == 0) {
        return;
    }

    // No interleaving case: direct copy
    if (config_.depth == 0 || config_.mode == config::TimeInterleaveMode::NONE) {
        std::copy(input, input + num_cells, output);
        ++blocks_processed_;
        return;
    }

    // Process each cell through its corresponding delay line
    for (size_t i = 0; i < num_cells; ++i) {
        output[i] = process_cell(input[i], i);
    }

    ++blocks_processed_;
}

void TimeDeinterleaver::process_batched(const int8_t* input, int8_t* output, size_t num_cells) {
    // Reserved for future double-buffering implementation
    process(input, output, num_cells);
}

int8_t TimeDeinterleaver::process_cell(int8_t input_cell, size_t cell_index) {
    // Determine which row this cell belongs to
    // Cells are distributed round-robin across rows
    size_t row;
    if (row_mask_ != 0) {
        // Fast path: bitmask for power-of-2 row counts
        row = cell_index & row_mask_;
    } else {
        // Standard path: modulo for other row counts
        row = cell_index % num_rows_;
    }

    // If this row has no delay, pass through directly
    if (delay_lines_[row].empty()) {
        return input_cell;
    }

    // Circular buffer operation: read old value, write new value
    size_t pos = write_pos_[row];
    int8_t output_cell = delay_lines_[row][pos];
    delay_lines_[row][pos] = input_cell;

    // Advance write position with wrap
    if (++pos >= delay_lines_[row].size()) {
        pos = 0;
    }
    write_pos_[row] = pos;

    return output_cell;
}

void TimeDeinterleaver::set_config(const TimeDeinterleaverConfig& config) {
    config_ = config;
    init_delay_lines();
}

void TimeDeinterleaver::reset() {
    // Clear all delay lines to zero
    for (auto& line : delay_lines_) {
        std::fill(line.begin(), line.end(), 0);
    }

    // Reset write positions
    std::fill(write_pos_.begin(), write_pos_.end(), 0);

    // Reset block counter
    blocks_processed_ = 0;
}

}  // namespace ofdm
}  // namespace atsc3
