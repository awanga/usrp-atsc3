#pragma once

// Pilot Extraction — ATSC 3.0 OFDM pilot symbol extraction
//
// Extracts scattered pilots (SP), continual pilots (CP), and edge pilots
// from FFT output for channel estimation and tracking.
//
// Pilot patterns PP1-PP8 per ATSC A/322 Section 7.2
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (FFT output, N_fft subcarriers)
//   Output: Vector of PilotSymbol (index, reference, received)
//
// Reference: ATSC A/322 Section 7.2 (Pilot Patterns)

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace ofdm {

// Pilot pattern enumeration (PP1-PP8)
enum class PilotPattern {
    PP1 = 1,  // Dx=3,  Dy=4
    PP2 = 2,  // Dx=6,  Dy=2
    PP3 = 3,  // Dx=6,  Dy=4
    PP4 = 4,  // Dx=12, Dy=2
    PP5 = 5,  // Dx=12, Dy=4
    PP6 = 6,  // Dx=24, Dy=2
    PP7 = 7,  // Dx=24, Dy=4
    PP8 = 8   // Dx=6,  Dy=16
};

// Pilot type classification
enum class PilotType {
    SCATTERED,  // Scattered pilot (SP) - varies by symbol
    CONTINUAL,  // Continual pilot (CP) - same position every symbol
    EDGE        // Edge pilot - at spectrum edges
};

// Single pilot observation
struct PilotSymbol {
    size_t subcarrier_index;    // FFT bin index
    sample_t reference_value;   // Known pilot value (BPSK sequence)
    sample_t received_value;    // Observed value from FFT output
    PilotType type;             // Type of pilot

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

    // Continual pilot count (depends on FFT size)
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
    PilotPattern pattern = PilotPattern::PP3;

    // Active carrier range (typically center 85-90% of FFT)
    // Set to 0 to auto-compute based on FFT size
    size_t first_active_carrier = 0;
    size_t num_active_carriers = 0;  // 0 = auto-compute from FFT size
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
//   config.pattern = PilotPattern::PP3;
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
    PilotPatternParams get_pattern_params() const;

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
    const PilotExtractorConfig& get_config() const { return config_; }

private:
    PilotExtractorConfig config_;

    // Cached pattern parameters
    PilotPatternParams params_;

    // Continual pilot positions (pre-computed for FFT size)
    std::vector<size_t> continual_positions_;

    // Pilot reference sequence (BPSK, derived from PRBS)
    std::vector<int8_t> pilot_sequence_;

    // Initialize internal state from config
    void init();

    // Compute pattern parameters
    void compute_pattern_params();

    // Generate continual pilot positions for current FFT size
    void generate_continual_positions();

    // Generate pilot reference sequence (PRBS-based)
    void generate_pilot_sequence();

    // Get PRBS bit for pilot at given position
    int8_t get_prbs_bit(size_t position) const;
};

// Utility: Get pattern parameters for a given pilot pattern
PilotPatternParams get_pilot_pattern_params(PilotPattern pattern, size_t fft_size);

// Utility: Get scattered pilot count for pattern
size_t get_scattered_pilot_count(PilotPattern pattern, size_t fft_size,
                                  size_t first_active, size_t num_active);

}  // namespace ofdm
}  // namespace atsc3
