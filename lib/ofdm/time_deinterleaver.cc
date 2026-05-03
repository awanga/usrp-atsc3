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

TimeDeinterleaver::TimeDeinterleaver(const TimeDeinterleaverConfig& config)
    : config_(config), num_rows_(0), settling_blocks_(0), blocks_processed_(0) {
    init_delay_lines();
}

TimeDeinterleaver::~TimeDeinterleaver() = default;

void TimeDeinterleaver::init_delay_lines() {
    // depth = 0 means no time interleaving
    if (config_.depth == 0 || config_.mode == config::TimeInterleaveMode::NONE) {
        num_rows_ = 1;
        settling_blocks_ = 0;
        delay_lines_.clear();
        write_pos_.clear();
        return;
    }

    // For CTI mode:
    // Number of rows = depth + 1
    // Row i has delay = i * cells_per_block for interleaver
    // De-interleaver has inverse delays: row i has delay = (num_rows - 1 - i) * cells_per_block
    num_rows_ = config_.depth + 1;

    delay_lines_.resize(num_rows_);
    write_pos_.resize(num_rows_, 0);

    // Pre-allocate delay lines
    // De-interleaver delay for row i = (num_rows - 1 - i) * cells_per_block
    for (size_t row = 0; row < num_rows_; ++row) {
        size_t delay = (num_rows_ - 1 - row) * config_.cells_per_block;
        if (delay > 0) {
            delay_lines_[row].resize(delay, 0);
        } else {
            delay_lines_[row].clear();
        }
    }

    // Calculate settling time (max delay in blocks)
    // The last row has zero delay, so max delay is from row 0
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

int8_t TimeDeinterleaver::process_cell(int8_t input_cell, size_t cell_index) {
    // Determine which row this cell belongs to
    // Cells are distributed round-robin across rows
    size_t row = cell_index % num_rows_;

    // If this row has no delay, pass through directly
    if (delay_lines_[row].empty()) {
        return input_cell;
    }

    // Circular buffer operation: read old value, write new value
    size_t pos = write_pos_[row];
    int8_t output_cell = delay_lines_[row][pos];
    delay_lines_[row][pos] = input_cell;

    // Advance write position
    write_pos_[row] = (pos + 1) % delay_lines_[row].size();

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
