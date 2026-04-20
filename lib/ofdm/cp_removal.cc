// cp_removal.cc — Cyclic Prefix Removal implementation
//
// Strips CP from OFDM symbols, outputting FFT-length windows

#include "cp_removal.h"

#include <algorithm>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

CpRemoval::CpRemoval(const CpRemovalConfig& config)
    : config_(config),
      cp_length_(compute_cp_length(config.fft_size, config.cp_fraction)),
      buffer_(cp_length_ + config.fft_size),
      buffer_idx_(0),
      callback_(nullptr),
      symbol_count_(0) {
    if (config.fft_size != 8192 && config.fft_size != 16384 && config.fft_size != 32768) {
        throw std::invalid_argument("Invalid FFT size for CP removal");
    }
}

CpRemoval::~CpRemoval() = default;

void CpRemoval::process(sample_t sample) {
    buffer_[buffer_idx_++] = sample;

    // Check if we have a complete symbol (CP + FFT)
    if (buffer_idx_ >= cp_length_ + config_.fft_size) {
        // Output the FFT portion (skip CP)
        if (callback_) {
            callback_(buffer_.data() + cp_length_, config_.fft_size);
        }
        ++symbol_count_;
        buffer_idx_ = 0;
    }
}

void CpRemoval::process(const sample_t* samples, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        process(samples[i]);
    }
}

void CpRemoval::set_output_callback(OutputCallback callback) {
    callback_ = std::move(callback);
}

void CpRemoval::reset() {
    buffer_idx_ = 0;
    symbol_count_ = 0;
}

void CpRemoval::reconfigure(const CpRemovalConfig& config) {
    if (config.fft_size != 8192 && config.fft_size != 16384 && config.fft_size != 32768) {
        throw std::invalid_argument("Invalid FFT size for CP removal");
    }

    config_ = config;
    cp_length_ = compute_cp_length(config.fft_size, config.cp_fraction);
    buffer_.resize(cp_length_ + config.fft_size);
    reset();
}

}  // namespace ofdm
}  // namespace atsc3
