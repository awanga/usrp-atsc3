#pragma once

// FFT Engine — OFDM FFT/IFFT processing
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (or ci16 fixed-point), TVALID, TREADY, TLAST(symbol boundary)
//   Output: TDATA=cf32 (or ci16 fixed-point), TVALID, TREADY, TLAST(symbol boundary)
//   Latency: 1 symbol (FFT_SIZE samples buffered before output)
//
// Supports:
//   - 8192, 16384, 32768 point transforms (ATSC 3.0 data symbols)
//   - 4096 point transform (bootstrap)
//   - FFTW3 backend (float mode) with wisdom caching
//   - Cooley-Tukey reference (fixed-point mode, no FFTW dependency)

#include "types.h"

#include <complex>
#include <cstddef>
#include <memory>
#include <string>

namespace atsc3 {
namespace ofdm {

// Supported FFT sizes
enum class FftSize : size_t {
    k4K = 4096,    // Bootstrap
    k8K = 8192,    // Data symbol
    k16K = 16384,  // Data symbol
    k32K = 32768   // Data symbol
};

// FFT direction
enum class FftDirection {
    kForward,  // Time -> Frequency (DFT)
    kInverse   // Frequency -> Time (IDFT)
};

// FFT Engine configuration
struct FftConfig {
    FftSize size = FftSize::k8K;
    FftDirection direction = FftDirection::kForward;

    // FFTW wisdom file path (empty = no persistence)
    // Wisdom speeds up subsequent FFT plan creation
    std::string wisdom_path;

    // Use patient planning (slower startup, faster execution)
    // Recommended for production, disable for quick testing
    bool use_patient_plan = false;

    // Normalize output by 1/N for inverse FFT
    bool normalize_inverse = true;
};

// FFT Engine interface
//
// Usage:
//   FftConfig config;
//   config.size = FftSize::k8K;
//   auto fft = FftEngine::create(config);
//
//   // Process one symbol
//   std::vector<sample_t> in(8192), out(8192);
//   fft->process(in.data(), out.data());
//
class FftEngine {
public:
    virtual ~FftEngine() = default;

    // Factory method - creates appropriate implementation based on build config
    static std::unique_ptr<FftEngine> create(const FftConfig& config);

    // Process one FFT/IFFT
    // in: input samples (size = fft_size)
    // out: output samples (size = fft_size)
    // Input and output buffers must not overlap
    virtual void process(const sample_t* in, sample_t* out) = 0;

    // Process with float interface (convenience for non-fixed-point paths)
    virtual void process(const std::complex<float>* in, std::complex<float>* out) = 0;

    // Get FFT size
    virtual size_t get_size() const = 0;

    // Get direction
    virtual FftDirection get_direction() const = 0;

    // Check if using FFTW backend
    virtual bool is_fftw_backend() const = 0;

protected:
    FftEngine() = default;
};

// Load FFTW wisdom from file
// Returns true if wisdom was loaded successfully
bool load_fftw_wisdom(const std::string& path);

// Save FFTW wisdom to file
// Returns true if wisdom was saved successfully
bool save_fftw_wisdom(const std::string& path);

}  // namespace ofdm
}  // namespace atsc3
