// pilot_extractor.cc — ATSC 3.0 OFDM pilot symbol extraction
//
// Extracts scattered pilots (SP), continual pilots (CP), and edge pilots
// from FFT output for channel estimation and tracking.
//
// Reference: ATSC A/322 Section 7.2 (Pilot Patterns)

#include "pilot_extractor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

namespace {

// Pilot pattern parameters table per ATSC A/322 Section 7.2
// Each pattern has frequency spacing (Dx) and time spacing (Dy)
struct PatternDef {
    size_t dx;  // Frequency spacing (subcarriers)
    size_t dy;  // Time spacing (symbols)
};

constexpr PatternDef kPatternTable[] = {
    {0, 0},   // Placeholder for index 0
    {3, 4},   // PP1: Dx=3, Dy=4
    {6, 2},   // PP2: Dx=6, Dy=2
    {6, 4},   // PP3: Dx=6, Dy=4
    {12, 2},  // PP4: Dx=12, Dy=2
    {12, 4},  // PP5: Dx=12, Dy=4
    {24, 2},  // PP6: Dx=24, Dy=2
    {24, 4},  // PP7: Dx=24, Dy=4
    {6, 16},  // PP8: Dx=6, Dy=16
};

// Active carrier counts per FFT size (from ATSC A/322)
constexpr size_t kActiveCarriers8K = 6913;
constexpr size_t kActiveCarriers16K = 13825;
constexpr size_t kActiveCarriers32K = 27649;

// Continual pilot positions for 8K FFT (ATSC A/322 Table 7.5)
// These are the carrier indices within the active region
const std::vector<size_t> kContinualPilots8K = {
    44,   122,  203,  262,  346,  410,  490,  559,  643,  715,  797,  866,  943,  1016, 1094, 1165,
    1247, 1318, 1397, 1469, 1550, 1619, 1700, 1769, 1850, 1922, 2000, 2069, 2153, 2222, 2299, 2378,
    2453, 2521, 2602, 2678, 2755, 2823, 2900, 2975, 3053, 3128, 3203, 3278, 3353, 3421, 3500, 3575,
    3650, 3721, 3800, 3878, 3950, 4022, 4097, 4171, 4250, 4318, 4397, 4469, 4547, 4618, 4694, 4769,
    4844, 4919, 4994, 5065, 5143, 5218, 5294, 5366, 5441, 5518, 5594, 5669, 5741, 5818, 5893, 5966,
    6044, 6116, 6197, 6262, 6343, 6412, 6493, 6562, 6643, 6715, 6793, 6869};

// Compute first active subcarrier (center FFT around DC)
size_t compute_first_active(size_t fft_size, size_t num_active) {
    return (fft_size - num_active) / 2;
}

// PRBS generator for pilot reference values
// Uses polynomial x^11 + x^2 + 1 per ATSC A/322
class PrbsGenerator {
public:
    PrbsGenerator() : state_(0x7FF) {}  // All ones initial state

    void reset() {
        state_ = 0x7FF;
    }

    int8_t next() {
        // Output is bit 0
        int8_t out = (state_ & 1) ? 1 : -1;

        // Feedback: x^11 + x^2 + 1
        int new_bit = ((state_ >> 10) ^ (state_ >> 1)) & 1;
        state_ = ((state_ << 1) | new_bit) & 0x7FF;

        return out;
    }

    // Get bit at specific position (generates sequence up to that point)
    int8_t at(size_t position) {
        reset();
        for (size_t i = 0; i < position; ++i) {
            next();
        }
        return next();
    }

private:
    uint32_t state_;
};

}  // namespace

// Maximum Q15 value (slightly less than 1.0 to avoid overflow)
constexpr float kQ15MaxValue = 0.999969482421875f;  // (32767/32768)

// Convert +/-1 integer to sample_t (handles Q15 range properly)
inline sample_t make_bpsk_sample(int8_t sign) {
#ifdef ATSC3_FIXED_POINT
    // Use 32767 for +1, -32767 for -1 to avoid overflow
    return sample_t(sign > 0 ? 32767 : -32767, 0);
#else
    return sample_t(static_cast<float>(sign), 0.0f);
#endif
}

// PilotSymbol methods
sample_t PilotSymbol::estimate_channel() const {
    // H = Y / X = received / reference
    // For BPSK pilots, reference is real-valued (+1 or -1)
#ifdef ATSC3_FIXED_POINT
    // Get sign of reference (+1 or -1)
    int ref_sign = (reference_value.real() > 0) ? 1 : -1;

    // H = Y * sign(X) for BPSK
    // Use 32-bit intermediate to avoid overflow
    int32_t h_re = static_cast<int32_t>(received_value.real()) * ref_sign;
    int32_t h_im = static_cast<int32_t>(received_value.imag()) * ref_sign;

    // Clamp to int16_t range
    h_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_re));
    h_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_im));

    return sample_t(static_cast<int16_t>(h_re), static_cast<int16_t>(h_im));
#else
    float ref = reference_value.real();
    return sample_t(received_value.real() * ref, received_value.imag() * ref);
#endif
}

double PilotSymbol::get_phase_error() const {
#ifdef ATSC3_FIXED_POINT
    float ref_re = q15_to_float(reference_value.real());
    float rec_re = q15_to_float(received_value.real());
    float rec_im = q15_to_float(received_value.imag());
#else
    float ref_re = reference_value.real();
    float rec_re = received_value.real();
    float rec_im = received_value.imag();
#endif

    // Phase error is angle of (received * conj(reference))
    // For BPSK reference, this simplifies to atan2(imag, real*ref)
    return std::atan2(rec_im * ref_re, rec_re * ref_re);
}

// PilotExtractor implementation
PilotExtractor::PilotExtractor(const PilotExtractorConfig& config) : config_(config) {
    init();
}

PilotExtractor::~PilotExtractor() = default;

void PilotExtractor::init() {
    compute_pattern_params();
    generate_continual_positions();
    generate_pilot_sequence();
}

void PilotExtractor::compute_pattern_params() {
    size_t idx = static_cast<size_t>(config_.pattern);
    if (idx < 1 || idx > 8) {
        throw std::invalid_argument("Invalid pilot pattern");
    }

    params_.pattern = config_.pattern;
    params_.dx = kPatternTable[idx].dx;
    params_.dy = kPatternTable[idx].dy;
    params_.has_edge_pilots = true;

    // Set active carrier range based on FFT size
    if (config_.fft_size == 8192) {
        params_.num_continual_pilots = kContinualPilots8K.size();
        if (config_.num_active_carriers == 0) {
            config_.num_active_carriers = kActiveCarriers8K;
        }
    } else if (config_.fft_size == 16384) {
        // 16K has approximately 2x as many continual pilots
        params_.num_continual_pilots = kContinualPilots8K.size() * 2;
        if (config_.num_active_carriers == 0) {
            config_.num_active_carriers = kActiveCarriers16K;
        }
    } else if (config_.fft_size == 32768) {
        // 32K has approximately 4x as many continual pilots
        params_.num_continual_pilots = kContinualPilots8K.size() * 4;
        if (config_.num_active_carriers == 0) {
            config_.num_active_carriers = kActiveCarriers32K;
        }
    } else {
        throw std::invalid_argument("Invalid FFT size for pilot extraction");
    }

    // Compute first active carrier if not set
    if (config_.first_active_carrier == 0) {
        config_.first_active_carrier =
            compute_first_active(config_.fft_size, config_.num_active_carriers);
    }

    params_.first_active = config_.first_active_carrier;
    params_.last_active = params_.first_active + config_.num_active_carriers - 1;
}

void PilotExtractor::generate_continual_positions() {
    continual_positions_.clear();

    // Scale factor for FFT size
    size_t scale = config_.fft_size / 8192;

    // Generate continual pilot positions
    // For larger FFT sizes, positions scale and additional pilots are added
    // to maintain consistent spectral density
    for (size_t pos : kContinualPilots8K) {
        // Scale position for FFT size
        size_t scaled_pos = pos * scale;
        size_t actual_pos = config_.first_active_carrier + scaled_pos;

        if (actual_pos <= params_.last_active) {
            continual_positions_.push_back(actual_pos);
        }
    }

    // For 16K and 32K, add interpolated positions to maintain pilot density
    if (scale > 1) {
        std::vector<size_t> additional;

        // Add midpoint positions between existing continual pilots
        for (size_t i = 0; i < kContinualPilots8K.size() - 1; ++i) {
            size_t pos1 = kContinualPilots8K[i] * scale;
            size_t pos2 = kContinualPilots8K[i + 1] * scale;

            // Add intermediate positions based on scale
            for (size_t j = 1; j < scale; ++j) {
                size_t interp_pos = pos1 + ((pos2 - pos1) * j) / scale;
                size_t actual_pos = config_.first_active_carrier + interp_pos;

                if (actual_pos <= params_.last_active) {
                    additional.push_back(actual_pos);
                }
            }
        }

        continual_positions_.insert(continual_positions_.end(), additional.begin(),
                                    additional.end());
    }

    // Sort and remove duplicates
    std::sort(continual_positions_.begin(), continual_positions_.end());
    auto last = std::unique(continual_positions_.begin(), continual_positions_.end());
    continual_positions_.erase(last, continual_positions_.end());
}

void PilotExtractor::generate_pilot_sequence() {
    // Pre-generate PRBS sequence for all possible pilot positions
    pilot_sequence_.resize(config_.fft_size);

    PrbsGenerator prbs;
    for (size_t i = 0; i < config_.fft_size; ++i) {
        pilot_sequence_[i] = prbs.next();
    }
}

int8_t PilotExtractor::get_prbs_bit(size_t position) const {
    if (position >= pilot_sequence_.size()) {
        return 1;  // Default to +1
    }
    return pilot_sequence_[position];
}

std::vector<PilotSymbol> PilotExtractor::extract(const sample_t* fft_output,
                                                 size_t symbol_index) const {
    std::vector<PilotSymbol> pilots;

    // Extract scattered pilots
    auto scattered = extract_scattered(fft_output, symbol_index);
    pilots.insert(pilots.end(), scattered.begin(), scattered.end());

    // Extract continual pilots
    auto continual = extract_continual(fft_output);
    pilots.insert(pilots.end(), continual.begin(), continual.end());

    // Extract edge pilots
    auto edge = extract_edge(fft_output);
    pilots.insert(pilots.end(), edge.begin(), edge.end());

    // Sort by subcarrier index
    std::sort(pilots.begin(), pilots.end(), [](const PilotSymbol& a, const PilotSymbol& b) {
        return a.subcarrier_index < b.subcarrier_index;
    });

    // Remove duplicates (some positions may be both scattered and continual)
    auto last =
        std::unique(pilots.begin(), pilots.end(), [](const PilotSymbol& a, const PilotSymbol& b) {
            return a.subcarrier_index == b.subcarrier_index;
        });
    pilots.erase(last, pilots.end());

    return pilots;
}

std::vector<PilotSymbol> PilotExtractor::extract_scattered(const sample_t* fft_output,
                                                           size_t symbol_index) const {
    std::vector<PilotSymbol> pilots;

    // Scattered pilot offset varies with symbol index
    // Offset = (symbol_index mod Dy) * Dx
    size_t offset = (symbol_index % params_.dy) * params_.dx;

    // Iterate through active carriers with Dx*Dy spacing, offset by symbol
    for (size_t k = params_.first_active + offset; k <= params_.last_active;
         k += params_.dx * params_.dy) {
        PilotSymbol pilot;
        pilot.subcarrier_index = k;
        pilot.received_value = fft_output[k];
        pilot.type = PilotType::SCATTERED;

        // Reference value from PRBS (use helper to avoid Q15 overflow)
        int8_t prbs = get_prbs_bit(k);
        pilot.reference_value = make_bpsk_sample(prbs);

        pilots.push_back(pilot);
    }

    return pilots;
}

std::vector<PilotSymbol> PilotExtractor::extract_continual(const sample_t* fft_output) const {
    std::vector<PilotSymbol> pilots;
    pilots.reserve(continual_positions_.size());

    for (size_t k : continual_positions_) {
        PilotSymbol pilot;
        pilot.subcarrier_index = k;
        pilot.received_value = fft_output[k];
        pilot.type = PilotType::CONTINUAL;

        // Reference value from PRBS (use helper to avoid Q15 overflow)
        int8_t prbs = get_prbs_bit(k);
        pilot.reference_value = make_bpsk_sample(prbs);

        pilots.push_back(pilot);
    }

    return pilots;
}

std::vector<PilotSymbol> PilotExtractor::extract_edge(const sample_t* fft_output) const {
    std::vector<PilotSymbol> pilots;

    if (!params_.has_edge_pilots) {
        return pilots;
    }

    // Edge pilots at first and last active carriers
    std::array<size_t, 2> edge_positions = {params_.first_active, params_.last_active};

    for (size_t k : edge_positions) {
        PilotSymbol pilot;
        pilot.subcarrier_index = k;
        pilot.received_value = fft_output[k];
        pilot.type = PilotType::EDGE;

        // Reference value from PRBS (use helper to avoid Q15 overflow)
        int8_t prbs = get_prbs_bit(k);
        pilot.reference_value = make_bpsk_sample(prbs);

        pilots.push_back(pilot);
    }

    return pilots;
}

PilotPatternParams PilotExtractor::get_pattern_params() const {
    return params_;
}

size_t PilotExtractor::get_scattered_pilot_count(size_t symbol_index) const {
    // Count scattered pilots for this symbol
    size_t offset = (symbol_index % params_.dy) * params_.dx;
    size_t count = 0;

    for (size_t k = params_.first_active + offset; k <= params_.last_active;
         k += params_.dx * params_.dy) {
        ++count;
    }

    return count;
}

size_t PilotExtractor::get_continual_pilot_count() const {
    return continual_positions_.size();
}

bool PilotExtractor::is_pilot(size_t subcarrier, size_t symbol_index) const {
    return is_scattered_pilot(subcarrier, symbol_index) || is_continual_pilot(subcarrier) ||
           subcarrier == params_.first_active || subcarrier == params_.last_active;
}

bool PilotExtractor::is_scattered_pilot(size_t subcarrier, size_t symbol_index) const {
    if (subcarrier < params_.first_active || subcarrier > params_.last_active) {
        return false;
    }

    size_t offset = (symbol_index % params_.dy) * params_.dx;
    size_t relative = subcarrier - params_.first_active;

    // Check if this position matches the scattered pilot pattern
    if (relative < offset) {
        return false;
    }

    return ((relative - offset) % (params_.dx * params_.dy)) == 0;
}

bool PilotExtractor::is_continual_pilot(size_t subcarrier) const {
    return std::binary_search(continual_positions_.begin(), continual_positions_.end(), subcarrier);
}

sample_t PilotExtractor::get_reference(size_t subcarrier, size_t /* symbol_index */) const {
    int8_t prbs = get_prbs_bit(subcarrier);
    return make_bpsk_sample(prbs);
}

void PilotExtractor::set_config(const PilotExtractorConfig& config) {
    config_ = config;
    init();
}

// Utility functions
PilotPatternParams get_pilot_pattern_params(PilotPattern pattern, size_t fft_size) {
    PilotExtractorConfig config;
    config.fft_size = fft_size;
    config.pattern = pattern;

    PilotExtractor extractor(config);
    return extractor.get_pattern_params();
}

size_t get_scattered_pilot_count(PilotPattern pattern, size_t fft_size, size_t first_active,
                                 size_t num_active) {
    PilotExtractorConfig config;
    config.fft_size = fft_size;
    config.pattern = pattern;
    config.first_active_carrier = first_active;
    config.num_active_carriers = num_active;

    PilotExtractor extractor(config);

    // Average over one complete Dy cycle
    PilotPatternParams params = extractor.get_pattern_params();
    size_t total = 0;
    for (size_t i = 0; i < params.dy; ++i) {
        total += extractor.get_scattered_pilot_count(i);
    }

    return total / params.dy;
}

}  // namespace ofdm
}  // namespace atsc3
