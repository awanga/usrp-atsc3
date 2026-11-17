// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// cp_removal.cc — Cyclic Prefix Removal implementation
//
// Strips CP from OFDM symbols, outputting FFT-length windows

#include "cp_removal.h"

#include <algorithm>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

size_t guard_interval_samples(GuardInterval gi) {
    switch (gi) {
        case GuardInterval::GI1_192:
            return 192;
        case GuardInterval::GI2_384:
            return 384;
        case GuardInterval::GI3_512:
            return 512;
        case GuardInterval::GI4_768:
            return 768;
        case GuardInterval::GI5_1024:
            return 1024;
        case GuardInterval::GI6_1536:
            return 1536;
        case GuardInterval::GI7_2048:
            return 2048;
        case GuardInterval::GI8_2432:
            return 2432;
        case GuardInterval::GI9_3072:
            return 3072;
        case GuardInterval::GI10_3648:
            return 3648;
        case GuardInterval::GI11_4096:
            return 4096;
        case GuardInterval::GI12_4864:
            return 4864;
    }
    return 0;
}

bool guard_interval_from_samples(size_t samples, GuardInterval* gi) {
    for (unsigned code = 1; code <= 12; ++code) {
        if (guard_interval_samples(static_cast<GuardInterval>(code)) == samples) {
            *gi = static_cast<GuardInterval>(code);
            return true;
        }
    }
    return false;
}

bool guard_interval_allowed(GuardInterval gi, size_t fft_size) {
    if (guard_interval_samples(gi) == 0) {
        return false;
    }
    switch (fft_size) {
        case 8192:
            return gi <= GuardInterval::GI7_2048;
        case 16384:
            return gi <= GuardInterval::GI11_4096;
        case 32768:
            return true;
        default:
            return false;
    }
}

namespace {

size_t checked_cp_length(const CpRemovalConfig& config) {
    if (!guard_interval_allowed(config.guard_interval, config.fft_size)) {
        throw std::invalid_argument("CpRemoval: FFT size and guard interval not allowed by A/322");
    }
    return guard_interval_samples(config.guard_interval);
}

}  // namespace

CpRemoval::CpRemoval(const CpRemovalConfig& config)
    : config_(config),
      cp_length_(checked_cp_length(config)),
      buffer_(cp_length_ + config.fft_size),
      buffer_idx_(0),
      callback_(nullptr),
      symbol_count_(0) {}

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
    cp_length_ = checked_cp_length(config);
    config_ = config;
    buffer_.resize(cp_length_ + config.fft_size);
    reset();
}

}  // namespace ofdm
}  // namespace atsc3
