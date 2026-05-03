#pragma once

// Constellation Demapper — ATSC 3.0 QAM symbol to soft LLR conversion
//
// Computes soft log-likelihood ratios (LLRs) from equalized QAM symbols
// for input to the LDPC decoder. Supports both uniform QAM and non-uniform
// constellations (NUC) per ATSC A/322 Section 7.5.
//
// Uses max-log approximation for efficient LLR computation:
//   LLR(b) = min_{s: b=0} |y-s|² - min_{s: b=1} |y-s|²
//
// Output LLRs are scaled by 1/noise_variance and clamped to int8_t [-127,+127].
//
// AXI4-S Interface Contract:
//   Input:  TDATA=sample_t (equalized QAM symbols)
//   Output: TDATA=int8_t (soft LLRs, bits_per_symbol per input symbol)
//           TVALID TREADY TLAST(codeword boundary)
//
// Reference: ATSC A/322 Section 7.5 (Non-Uniform Constellations)

#include "config/atsc3_config.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Demapper configuration
struct DemapperConfig {
    // Modulation type (from L1 signaling)
    config::Modulation modulation = config::Modulation::QAM64;

    // LDPC code rate (needed for NUC table selection)
    config::CodeRate code_rate = config::CodeRate::RATE_7_15;

    // Noise variance estimate for LLR scaling
    // Higher = more aggressive scaling, lower = more conservative
    float noise_variance = 0.1f;

    // Use max-log approximation (true) or exact LLR (false)
    // Max-log is faster and sufficient for most SNR conditions
    bool use_max_log = true;

    // LLR clipping value (magnitude)
    int8_t llr_clip = 127;
};

// Demapping result for one OFDM symbol
struct DemapResult {
    // Soft LLRs for all bits
    std::vector<int8_t> llr;

    // Number of symbols demapped
    size_t num_symbols = 0;

    // Bits per symbol used
    size_t bits_per_symbol = 0;

    // Valid flag
    bool is_valid = false;
};

// Constellation Demapper
//
// Converts equalized QAM symbols to soft LLRs for LDPC decoding.
// Pre-computes constellation points at construction for efficiency.
//
// Usage:
//   DemapperConfig config;
//   config.modulation = Modulation::QAM64;
//   config.noise_variance = 0.05f;
//   ConstellationDemapper demapper(config);
//
//   // Demap equalized symbols
//   DemapResult result = demapper.demap(equalized.data(), num_symbols);
//
//   // Feed LLRs to LDPC decoder
//   ldpc.decode(result.llr.data(), ...);
//
class ConstellationDemapper {
public:
    explicit ConstellationDemapper(const DemapperConfig& config = DemapperConfig());
    ~ConstellationDemapper();

    // Demap equalized symbols to soft LLRs
    // symbols: Equalized QAM symbols (num_symbols samples)
    // num_symbols: Number of symbols to demap
    // Returns: Soft LLRs (num_symbols * bits_per_symbol bytes)
    DemapResult demap(const sample_t* symbols, size_t num_symbols) const;

    // Demap into pre-allocated buffer (no allocation)
    // llr_out: Output buffer (must have room for num_symbols * bits_per_symbol)
    // Returns: Number of LLRs written
    size_t demap(const sample_t* symbols, size_t num_symbols, int8_t* llr_out) const;

    // Demap a single symbol
    // symbol: Equalized QAM symbol
    // llr_out: Output buffer for bits_per_symbol LLRs
    void demap_symbol(sample_t symbol, int8_t* llr_out) const;

    // Get bits per symbol for current modulation
    size_t bits_per_symbol() const {
        return bits_per_symbol_;
    }

    // Get number of constellation points
    size_t constellation_size() const {
        return constellation_points_.size();
    }

    // Get constellation points (for visualization/debugging)
    const std::vector<sample_t>& constellation_points() const {
        return constellation_points_;
    }

    // Update configuration
    void set_config(const DemapperConfig& config);
    const DemapperConfig& get_config() const {
        return config_;
    }

    // Reset internal state
    void reset();

private:
    DemapperConfig config_;

    // Bits per symbol for current modulation
    size_t bits_per_symbol_;

    // Pre-computed constellation points (ordered by Gray-mapped bit pattern)
    std::vector<sample_t> constellation_points_;

    // Bit mappings for constellation points
    // bit_sets_[bit_index] contains indices of points where that bit is 1
    // bit_clears_[bit_index] contains indices of points where that bit is 0
    std::vector<std::vector<size_t>> bit_sets_;
    std::vector<std::vector<size_t>> bit_clears_;

    // LLR scaling factor (1.0 / noise_variance)
    float llr_scale_;

    // Initialize from config
    void init();

    // Generate uniform QAM constellation
    void generate_uniform_constellation();

    // Generate NUC constellation (loaded from tables)
    void generate_nuc_constellation();

    // Build bit mapping tables
    void build_bit_mappings();

    // Compute LLR for a single bit using max-log approximation
    // symbol: Received symbol
    // bit_index: Which bit (0 = MSB)
    // Returns: LLR value (clamped to int8_t range)
    int8_t compute_llr_max_log(sample_t symbol, size_t bit_index) const;

    // Compute squared distance between two samples
    float squared_distance(sample_t a, sample_t b) const;
};

// Check if modulation is a NUC type
inline bool is_nuc_modulation(config::Modulation mod) {
    return mod >= config::Modulation::NUC_QPSK;
}

// Get bits per symbol for a modulation type
inline size_t get_bits_per_symbol(config::Modulation mod) {
    return config::Atsc3Config::bits_per_symbol(mod);
}

// Get constellation size for a modulation type (2^bits_per_symbol)
inline size_t get_constellation_size(config::Modulation mod) {
    return 1u << get_bits_per_symbol(mod);
}

}  // namespace ofdm
}  // namespace atsc3
