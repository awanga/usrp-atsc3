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

    // LLR scaling factor (1.0 / noise_variance). Phase 9.0b: under
    // ATSC3_FIXED_POINT this is a Q16.16-ish fixed-point value (generous
    // range -- noise_variance, and so this scale, can span several orders
    // of magnitude), not a float; float builds are unchanged.
#ifdef ATSC3_FIXED_POINT
    int32_t llr_scale_q16_;

    // Precomputed for demap_uniform_fixed() (uniform QAM only): half the
    // per-axis grid side (e.g. 4 for 64-QAM's 8-level axis) and the
    // Q1.15-quantized, headroom-adjusted per-axis normalization constant
    // for the current modulation. Set in init(); 0/unused for NUC
    // modulations, which stay on compute_llr_max_log().
    int half_side_;
    int16_t norm_const_q15_;

    // Per-axis-bit decision tables for bits 1..bits_per_axis-1 (bit 0, the
    // axis MSB, needs no table -- it's the sign of the coordinate,
    // unconditionally). Index 0 of each outer vector is unused/empty.
    // uniform_cell_bit_[b][c] is the Gray-coded bit value of axis-bit b
    // when the received coordinate's magnitude falls in cell c (c = 0 is
    // the innermost grid level, c = half_side_-1 the outermost).
    // uniform_boundaries_[b] lists every cell-boundary position (Q1.15,
    // magnitude units) where bit b's value actually changes between
    // adjacent cells -- built directly from to_gray()/from_gray() (see
    // constellation_demapper.cc) rather than a closed-form zone formula,
    // because Gray-coded PAM's deeper bits (b >= 1) do not follow the
    // simple periodic pattern a naive "reduced mod period" zone split
    // assumes; that was tried first and verified wrong against this exact
    // codebase's from_gray() (a real, non-obvious bug, not a style
    // choice -- see the detailed derivation in the .cc).
    std::vector<std::vector<uint8_t>> uniform_cell_bit_;
    std::vector<std::vector<int32_t>> uniform_boundaries_;
#else
    float llr_scale_;
#endif

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

    // Compute squared distance between two samples. Under
    // ATSC3_FIXED_POINT this is genuine integer arithmetic returning a
    // raw (unshifted) Q1.15 x Q1.15 squared-distance sum -- used only for
    // MIN comparisons and one subtraction in compute_llr_max_log() below,
    // both scale-invariant, so there's no need to rescale per call.
#ifdef ATSC3_FIXED_POINT
    int64_t squared_distance(sample_t a, sample_t b) const;
#else
    float squared_distance(sample_t a, sample_t b) const;
#endif

#ifdef ATSC3_FIXED_POINT
    // Genuine fixed-point boundary slicer for uniform (Gray-coded,
    // separable) QAM -- Phase 9.0b. Replaces both the hand-unrolled
    // demap_qamXX_fast() functions (QPSK/16/64/256) and, for uniform
    // 1024/4096-QAM (which previously had no fast path at all), the O(M)
    // compute_llr_max_log() fallback. Writes bits_per_symbol_ LLRs to out.
    void demap_uniform_fixed(sample_t symbol, int8_t* out) const;

    // Signed distance (Q1.15) from one received axis coordinate to the
    // nearest decision boundary for axis-bit bit_index, positive meaning
    // bit=0 is more likely. bit_index 0 (axis MSB) is just -coord_q15;
    // bit_index >= 1 uses the uniform_cell_bit_/uniform_boundaries_
    // tables built in init() (see the header comment on those members
    // for why a closed-form formula doesn't work here).
    int32_t axis_bit_distance(int16_t coord_q15, size_t bit_index) const;
#endif
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
