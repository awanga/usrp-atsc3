#pragma once

// Frequency De-interleaver — ATSC 3.0 frequency-domain de-interleaving
//
// Reverses the frequency interleaving applied at the transmitter, which
// spreads data across OFDM subcarriers to improve frequency-selective
// fading resilience.
//
// The permutation is based on a pseudo-random sequence that depends on
// FFT size and number of active carriers per ATSC A/322 Section 8.3.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=int8_t (interleaved LLR cells, one OFDM symbol)
//   Output: TDATA=int8_t (de-interleaved LLR cells)
//           TVALID TREADY TLAST(symbol boundary)
//
// Reference: ATSC A/322 Section 8.3 (Frequency Interleaving)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Frequency de-interleaver configuration
struct FreqDeinterleaverConfig {
    // FFT size (8192, 16384, or 32768)
    size_t fft_size = 8192;

    // Number of active (data + pilot) carriers
    size_t num_active_carriers = 6913;
};

// Frequency De-interleaver
//
// Applies inverse frequency permutation to recover original carrier ordering.
// Permutation is pre-computed at construction for efficiency.
//
// The interleaver uses a pseudo-random permutation based on:
//   - FFT size (determines maximum addressable carriers)
//   - Number of active carriers (actual data carriers used)
//
// Usage:
//   FreqDeinterleaverConfig config;
//   config.fft_size = 8192;
//   config.num_active_carriers = 6913;
//   FreqDeinterleaver deint(config);
//
//   // De-interleave one OFDM symbol
//   deint.deinterleave(input, output);
//
class FreqDeinterleaver {
public:
    explicit FreqDeinterleaver(const FreqDeinterleaverConfig& config = FreqDeinterleaverConfig());
    ~FreqDeinterleaver();

    // De-interleave one OFDM symbol (out-of-place)
    // input: Interleaved carriers (num_active_carriers elements)
    // output: De-interleaved carriers (num_active_carriers elements)
    void deinterleave(const int8_t* input, int8_t* output) const;

    // De-interleave in-place
    // carriers: Array of carriers to de-interleave
    void deinterleave_inplace(int8_t* carriers);

    // Get current configuration
    const FreqDeinterleaverConfig& get_config() const {
        return config_;
    }

    // Update configuration (recomputes permutation)
    void set_config(const FreqDeinterleaverConfig& config);

    // Get the inverse permutation table
    const std::vector<size_t>& get_permutation() const {
        return inv_permutation_;
    }

    // Get number of active carriers
    size_t num_carriers() const {
        return config_.num_active_carriers;
    }

    // Reset internal state
    void reset();

private:
    FreqDeinterleaverConfig config_;

    // Inverse permutation: inv_permutation_[i] = source index for output position i
    std::vector<size_t> inv_permutation_;

    // Temporary buffer for in-place de-interleaving
    std::vector<int8_t> temp_buffer_;

    // Compute the permutation for given configuration
    void compute_permutation();

    // ATSC A/322 frequency interleaver pseudo-random sequence generator
    // Returns the interleaved position for a given input position
    size_t interleaver_permutation(size_t input_pos) const;
};

// Compute the frequency interleaver output position for testing
// Returns the output position for a given input position
size_t compute_freq_interleaver_position(size_t input_pos, size_t fft_size, size_t num_carriers);

}  // namespace ofdm
}  // namespace atsc3
