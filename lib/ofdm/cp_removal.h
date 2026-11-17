// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// CP Removal — Cyclic Prefix Stripping for OFDM Symbols
//
// Removes the cyclic prefix from incoming OFDM symbols to extract
// the FFT window. The guard interval is signaled in L1 (A/322 Table
// 9.13); its length in samples is fixed by A/322 Table 8.9, whatever the
// FFT size. Time-aligned frames add excess samples to the guard
// intervals (A/322 Section 8.5.1); that is not handled here yet.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (streaming samples with CP)
//   Output: TDATA=cf32, TVALID, TREADY, TLAST (FFT-length symbols)
//
// Reference: ATSC A/322:2026 Section 8.5, Tables 8.9 and 9.13

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Guard interval patterns, numbered by their A/322 Table 9.13 code
// (L1B_first_sub_guard_interval, L1D_guard_interval); codes 0 and 13-15
// are reserved.
enum class GuardInterval : uint8_t {
    GI1_192 = 1,
    GI2_384 = 2,
    GI3_512 = 3,
    GI4_768 = 4,
    GI5_1024 = 5,
    GI6_1536 = 6,
    GI7_2048 = 7,
    GI8_2432 = 8,
    GI9_3072 = 9,
    GI10_3648 = 10,
    GI11_4096 = 11,
    GI12_4864 = 12
};

// A/322 Table 8.9: the guard interval length in samples, the same for
// every FFT size; 0 for a reserved code.
size_t guard_interval_samples(GuardInterval gi);

// The pattern with that length in samples; false if Table 8.9 has none.
bool guard_interval_from_samples(size_t samples, GuardInterval* gi);

// A/322 Table 8.9: whether gi may be used with fft_size (GI8-GI11 need
// 16K or 32K, GI12 needs 32K). False for a reserved code or an FFT size
// other than 8192, 16384, 32768.
bool guard_interval_allowed(GuardInterval gi, size_t fft_size);

// CP Removal configuration
struct CpRemovalConfig {
    // FFT size (8192, 16384, 32768)
    size_t fft_size = 8192;

    GuardInterval guard_interval = GuardInterval::GI5_1024;
};

// CP Removal processor
//
// Usage:
//   CpRemovalConfig config;
//   config.fft_size = 8192;
//   config.guard_interval = GuardInterval::GI5_1024;  // 1024 samples
//   CpRemoval cp_remover(config);
//
//   // Set callback for complete symbols
//   cp_remover.set_output_callback([](const sample_t* symbol, size_t len) {
//       // Process FFT-length symbol
//   });
//
//   // Feed samples
//   cp_remover.process(samples, n);
//
class CpRemoval {
public:
    // Callback for complete symbols (FFT length)
    using OutputCallback = std::function<void(const sample_t* symbol, size_t len)>;

    // Throws std::invalid_argument unless guard_interval_allowed().
    explicit CpRemoval(const CpRemovalConfig& config);
    ~CpRemoval();

    // Process input samples
    // Calls output callback for each complete symbol
    void process(const sample_t* samples, size_t n);

    // Process single sample
    void process(sample_t sample);

    // Set output callback
    void set_output_callback(OutputCallback callback);

    // Reset state (flush buffer)
    void reset();

    // Reconfigure (resets state)
    void reconfigure(const CpRemovalConfig& config);

    // Get current configuration
    const CpRemovalConfig& get_config() const {
        return config_;
    }

    // Get computed CP length
    size_t get_cp_length() const {
        return cp_length_;
    }

    // Get symbol length (CP + FFT)
    size_t get_symbol_length() const {
        return cp_length_ + config_.fft_size;
    }

    // Get count of complete symbols output
    size_t get_symbol_count() const {
        return symbol_count_;
    }

private:
    CpRemovalConfig config_;
    size_t cp_length_;

    // Input buffer (accumulates one full symbol)
    std::vector<sample_t> buffer_;
    size_t buffer_idx_;

    OutputCallback callback_;
    size_t symbol_count_;
};

}  // namespace ofdm
}  // namespace atsc3
