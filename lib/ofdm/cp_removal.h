#pragma once

// CP Removal — Cyclic Prefix Stripping for OFDM Symbols
//
// Removes the cyclic prefix from incoming OFDM symbols to extract
// the FFT window. CP length is configurable based on L1 signaling.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (streaming samples with CP)
//   Output: TDATA=cf32, TVALID, TREADY, TLAST (FFT-length symbols)
//
// Reference: ATSC A/322 Section 7

#include "types.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Supported CP fractions (as numerator/8192, scaled by FFT size)
enum class CpFraction : size_t {
    k192_8192 = 192,    // 1/42.67
    k384_8192 = 384,    // 1/21.33
    k512_8192 = 512,    // 1/16
    k768_8192 = 768,    // 1/10.67
    k1024_8192 = 1024,  // 1/8
    k1536_8192 = 1536,  // 1/5.33
    k2048_8192 = 2048,  // 1/4
    k2432_8192 = 2432,  // 19/64
    k3072_8192 = 3072,  // 3/8
    k3648_8192 = 3648,  // 57/128
    k4096_8192 = 4096   // 1/2
};

// CP Removal configuration
struct CpRemovalConfig {
    // FFT size (8192, 16384, 32768)
    size_t fft_size = 8192;

    // CP fraction numerator (over 8192)
    CpFraction cp_fraction = CpFraction::k1024_8192;
};

// Compute CP length from configuration
inline size_t compute_cp_length(size_t fft_size, CpFraction cp_fraction) {
    // CP length = fft_size * (cp_fraction / 8192)
    return (fft_size * static_cast<size_t>(cp_fraction)) / 8192;
}

// CP Removal processor
//
// Usage:
//   CpRemovalConfig config;
//   config.fft_size = 8192;
//   config.cp_fraction = CpFraction::k1024_8192;  // CP = 1024 samples
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
    const CpRemovalConfig& get_config() const { return config_; }

    // Get computed CP length
    size_t get_cp_length() const { return cp_length_; }

    // Get symbol length (CP + FFT)
    size_t get_symbol_length() const { return cp_length_ + config_.fft_size; }

    // Get count of complete symbols output
    size_t get_symbol_count() const { return symbol_count_; }

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
