// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Pilot Extraction — ATSC 3.0 OFDM pilot symbol extraction
//
// Extracts scattered pilots (SP), continual pilots (CP), and edge pilots
// from FFT output for channel estimation and tracking.
//
// Pilot layout per ATSC A/322:2018 Section 8.1 and Annexes D-E: the 16
// scattered pilot patterns SPa_b, the common CP sets derived from CP32,
// the additional scattered-pilot-bearing CPs, edge pilots at relative
// carriers 0 and NoC-1, and the 13-bit reference sequence reloaded at the
// start of every symbol. Preamble and subframe-boundary symbols carry
// other pilot sets and are not handled here.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (FFT output, N_fft subcarriers)
//   Output: Vector of PilotSymbol (index, reference, received)
//
// Reference: ATSC A/322 Section 8.1 (Pilot Insertion), Annex D, Annex E
//
#include "config/atsc3_config.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Scattered pilot patterns SPa_b (DX = a, DY = b, Table 8.2), numbered by their
// L1D_scattered_pilot_pattern code (Table 9.12); defined with the L1 configuration.
using PilotPattern = config::PilotPattern;

// Pilot type classification
enum class PilotType {
    SCATTERED,  // Scattered pilot (SP) - varies by symbol
    CONTINUAL,  // Continual pilot (CP) - same position every symbol
    EDGE        // Edge pilot - at spectrum edges
};

// Single pilot observation
struct PilotSymbol {
    size_t subcarrier_index;   // FFT bin index
    sample_t reference_value;  // Known pilot value (BPSK sequence)
    sample_t received_value;   // Observed value from FFT output
    PilotType type;            // Type of pilot

    // Compute channel estimate at this pilot (H = Y / X)
    sample_t estimate_channel() const;

    // Get phase error (for tracking)
    double get_phase_error() const;
};

// Pilot pattern parameters
struct PilotPatternParams {
    PilotPattern pattern;

    // Scattered pilot spacing
    size_t dx;  // Frequency spacing (subcarriers)
    size_t dy;  // Time spacing (symbols)

    // Carrier reduction coefficient implied by the number of active carriers
    size_t reduced_carriers;

    // Continual pilot count: common plus additional CPs inside the active window
    size_t num_continual_pilots;

    // Edge pilot info
    bool has_edge_pilots;

    // Active subcarriers
    size_t first_active;  // First active subcarrier
    size_t last_active;   // Last active subcarrier
};

// Pilot extractor configuration
struct PilotExtractorConfig {
    // FFT size
    size_t fft_size = 8192;

    // Pilot pattern
    PilotPattern pattern = PilotPattern::SP6_4;

    // Active carrier range. The number of carriers is NoC = NoCmax -
    // Cred_coeff * Cunit (Table 7.1), so it must be one of the five legal
    // values for the FFT size (8K: 6913 down to 6529 in steps of 96, 16K:
    // 13825 in steps of 192, 32K: 27649 in steps of 384); 0 selects NoCmax.
    // The window is centered in the FFT unless first_active_carrier is
    // set (0 = centered).
    size_t first_active_carrier = 0;
    size_t num_active_carriers = 0;
};

// Pilot Extractor
//
// Extracts pilot symbols from FFT output for channel estimation.
// Generates both scattered and continual pilot locations based on
// ATSC 3.0 pilot patterns.
//
// Usage:
//   PilotExtractorConfig config;
//   config.fft_size = 8192;
//   config.pattern = PilotPattern::SP6_4;
//   PilotExtractor extractor(config);
//
//   // Extract pilots from FFT output
//   std::vector<PilotSymbol> pilots = extractor.extract(fft_output, symbol_index);
//
//   // Get just continual pilots (for CFO tracking)
//   std::vector<PilotSymbol> cp = extractor.extract_continual(fft_output);
//
class PilotExtractor {
public:
    explicit PilotExtractor(const PilotExtractorConfig& config = PilotExtractorConfig());
    ~PilotExtractor();

    // Extract all pilots from FFT output
    // fft_output: FFT output buffer (size = fft_size)
    // symbol_index: OFDM symbol index within frame (for scattered pilot pattern)
    // Returns: vector of pilot observations
    std::vector<PilotSymbol> extract(const sample_t* fft_output, size_t symbol_index) const;

    // Extract only scattered pilots
    std::vector<PilotSymbol> extract_scattered(const sample_t* fft_output,
                                               size_t symbol_index) const;

    // Extract only continual pilots
    std::vector<PilotSymbol> extract_continual(const sample_t* fft_output) const;

    // Extract only edge pilots
    std::vector<PilotSymbol> extract_edge(const sample_t* fft_output) const;

    // Get pilot pattern parameters
    const PilotPatternParams& get_pattern_params() const;

    // Get number of scattered pilots per symbol
    size_t get_scattered_pilot_count(size_t symbol_index) const;

    // Get number of continual pilots
    size_t get_continual_pilot_count() const;

    // Check if a subcarrier is a pilot (any type)
    bool is_pilot(size_t subcarrier, size_t symbol_index) const;

    // Check if a subcarrier is a scattered pilot
    bool is_scattered_pilot(size_t subcarrier, size_t symbol_index) const;

    // Check if a subcarrier is a continual pilot
    bool is_continual_pilot(size_t subcarrier) const;

    // Get reference value for a pilot
    sample_t get_reference(size_t subcarrier, size_t symbol_index) const;

    // Update configuration
    void set_config(const PilotExtractorConfig& config);

    // Get current configuration
    const PilotExtractorConfig& get_config() const {
        return config_;
    }

private:
    PilotExtractorConfig config_;

    // Cached pattern parameters
    PilotPatternParams params_;

    // Continual pilot positions (pre-computed for FFT size)
    std::vector<size_t> continual_positions_;

    // Reference sequence r_k as +1 / -1 (r_k = 0 / 1), indexed by relative carrier
    std::vector<int8_t> pilot_sequence_;

    // Initialize internal state from config
    void init();

    // Compute pattern parameters
    void compute_pattern_params();

    // Generate continual pilot positions for current FFT size
    void generate_continual_positions();

    // Generate pilot reference sequence (PRBS-based)
    void generate_pilot_sequence();

    // Reference sign (+1 / -1) for relative carrier index k
    int8_t get_prbs_bit(size_t relative_carrier) const;
};

// Common continual pilot set for an FFT size, ascending absolute carrier
// indices (Section 8.1.4.1: CP32 for 32K, every 2nd / 4th entry halved /
// quartered, rounded up, for 16K / 8K). Used to generate the RTL's ROM.
std::vector<size_t> common_continual_pilots(size_t fft_size);

// Whether Annex D defines a pilot layout for the pattern at this FFT size
bool pilot_pattern_allowed(size_t fft_size, PilotPattern pattern);

// Additional scattered-pilot-bearing continual pilots (Tables D.1.4 and
// D.1.5): relative carrier indices for the carrier reduction coefficient,
// ascending, with the odd-reduction exclusions applied. Throws
// std::invalid_argument if the pattern is not allowed at this FFT size.
std::vector<size_t> additional_continual_pilots(size_t fft_size, PilotPattern pattern,
                                                size_t reduced_carriers);

// Utility: Get pattern parameters for a given pilot pattern
PilotPatternParams get_pilot_pattern_params(PilotPattern pattern, size_t fft_size);

// Utility: Get scattered pilot count for pattern
size_t get_scattered_pilot_count(PilotPattern pattern, size_t fft_size, size_t first_active,
                                 size_t num_active);

}  // namespace ofdm
}  // namespace atsc3
