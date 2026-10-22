// pilot_extractor.cc — ATSC 3.0 OFDM pilot symbol extraction
//
// Extracts scattered pilots (SP), continual pilots (CP), and edge pilots
// from FFT output for channel estimation and tracking.
//
// Reference: ATSC A/322:2018 Section 8.1 (Pilot Insertion), Annex D
// (Continual Pilot Patterns). The CP32 table and the additional-CP
// indices below are transcribed from Tables D.1.1, D.1.4 and D.1.5.

#include "pilot_extractor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

namespace {

// Scattered pilot spacing DX and pattern depth DY per pattern code (Table 8.2)
struct PatternDef {
    size_t dx;  // Frequency spacing (carriers)
    size_t dy;  // Time spacing (symbols)
};

constexpr PatternDef kPatternTable[] = {
    {3, 2},  {3, 4},  {4, 2},  {4, 4},  {6, 2},  {6, 4},  {8, 2},  {8, 4},
    {12, 2}, {12, 4}, {16, 2}, {16, 4}, {24, 2}, {24, 4}, {32, 2}, {32, 4},
};
constexpr size_t kNumPatterns = sizeof(kPatternTable) / sizeof(kPatternTable[0]);

// Maximum carriers NoCmax and carrier reduction unit Cunit per FFT size
// (Section 7.2.3, Table 7.1)
struct FftDef {
    size_t fft_size;
    size_t noc_max;
    size_t c_unit;
};

constexpr FftDef kFftTable[] = {
    {8192, 6913, 96},
    {16384, 13825, 192},
    {32768, 27649, 384},
};
constexpr size_t kMaxReduction = 4;  // Cred_coeff ranges 0..4

// Common continual pilot set CP32: absolute carrier indices, Table D.1.1
constexpr size_t kCp32[192] = {
    236,   316,   356,   412,   668,   716,   868,   1100,  1228,  1268,  1340,  1396,  1876,
    1916,  2140,  2236,  2548,  2644,  2716,  2860,  3004,  3164,  3236,  3436,  3460,  3700,
    3836,  4028,  4124,  4132,  4156,  4316,  4636,  5012,  5132,  5140,  5332,  5372,  5500,
    5524,  5788,  6004,  6020,  6092,  6428,  6452,  6500,  6740,  7244,  7316,  7372,  7444,
    7772,  7844,  7924,  8020,  8164,  8308,  8332,  8348,  8788,  8804,  9116,  9140,  9292,
    9412,  9436,  9604,  10076, 10204, 10340, 10348, 10420, 10660, 10684, 10708, 11068, 11132,
    11228, 11356, 11852, 11860, 11884, 12044, 12116, 12164, 12268, 12316, 12700, 12772, 12820,
    12988, 13300, 13340, 13564, 13780, 13868, 14084, 14308, 14348, 14660, 14828, 14876, 14948,
    15332, 15380, 15484, 15532, 15604, 15764, 15788, 15796, 16292, 16420, 16516, 16580, 16940,
    16964, 16988, 17228, 17300, 17308, 17444, 17572, 18044, 18212, 18236, 18356, 18508, 18532,
    18844, 18860, 19300, 19316, 19340, 19484, 19628, 19724, 19804, 19876, 20204, 20276, 20332,
    20404, 20908, 21148, 21196, 21220, 21556, 21628, 21644, 21860, 22124, 22148, 22276, 22316,
    22508, 22516, 22636, 23012, 23332, 23492, 23516, 23524, 23620, 23812, 23948, 24188, 24212,
    24412, 24484, 24644, 24788, 24932, 25004, 25100, 25412, 25508, 25732, 25772, 26252, 26308,
    26380, 26420, 26548, 26780, 26932, 26980, 27236, 27292, 27332, 27412,
};

// Additional scattered-pilot-bearing continual pilots, relative carrier
// indices (Tables D.1.4 and D.1.5). Entries in parentheses in the standard
// are not used when Cred_coeff is odd (`odd_unused`). A pattern that has no
// entry for an FFT size is not an allowed combination (Table 8.3).
struct AdditionalCp {
    size_t index;
    bool odd_unused;
};

struct AdditionalSet {
    bool allowed;
    size_t count;
    AdditionalCp cp[3];
};

constexpr AdditionalSet kNone = {false, 0, {{0, false}, {0, false}, {0, false}}};
// Allowed, but the set is computed (8K SP32_4)
constexpr AdditionalSet kByReduction = {true, 0, {{0, false}, {0, false}, {0, false}}};

constexpr AdditionalSet one(size_t a, bool unused = false) {
    return {true, 1, {{a, unused}, {0, false}, {0, false}}};
}

constexpr AdditionalSet three(size_t a, size_t b, size_t c, bool b_unused = false,
                              bool c_unused = false) {
    return {true, 3, {{a, false}, {b, b_unused}, {c, c_unused}}};
}

// [pattern code][fft: 0 = 8K, 1 = 16K, 2 = 32K]
constexpr AdditionalSet kAdditional[kNumPatterns][3] = {
    /* SP3_2  */ {one(1731), one(3471), one(6939)},
    /* SP3_4  */ {three(1731, 2886, 5733), three(3471, 5778, 11469), kNone},
    /* SP4_2  */ {one(1732), one(3460), kNone},
    /* SP4_4  */ {three(1732, 2888, 5724), three(3460, 5768, 11452), kNone},
    /* SP6_2  */ {one(1734), one(3462), one(6942)},
    /* SP6_4  */ {three(1734, 2892, 5730), three(3462, 5772, 11466), kNone},
    /* SP8_2  */ {one(1736), one(3464), one(6920)},
    /* SP8_4  */ {three(1736, 2896, 5720), three(3464, 5776, 11448), kNone},
    /* SP12_2 */ {one(1740), one(3468), one(6924)},
    /* SP12_4 */ {three(1740, 2904, 5748), three(3468, 5784, 11460), kNone},
    /* SP16_2 */ {one(1744), one(3472), one(6928)},
    /* SP16_4 */ {three(1744, 2912, 5744, true, true), three(3472, 5792, 11440), kNone},
    /* SP24_2 */ {kNone, one(3480), one(6936)},
    /* SP24_4 */ {kNone, three(3480, 5808, 11496), kNone},
    /* SP32_2 */ {one(1696, true), one(3488), one(6944)},
    /* SP32_4 */ {kByReduction, three(3488, 5824, 11488, true, true), kNone},
};

// 8K SP32_4 additional CPs depend on Cred_coeff (Table D.1.5)
std::vector<size_t> additional_sp32_4_8k(size_t cred) {
    switch (cred) {
        case 0:
            return {1696, 2880, 5728};
        case 1:
            return {};
        case 2:
            return {1696};
        case 3:
            return {1696, 2880};
        default:
            return {1696, 2880, 5728};
    }
}

const FftDef& fft_def(size_t fft_size) {
    for (const FftDef& def : kFftTable) {
        if (def.fft_size == fft_size) {
            return def;
        }
    }
    throw std::invalid_argument("Invalid FFT size for pilot extraction");
}

size_t fft_index(size_t fft_size) {
    return static_cast<size_t>(&fft_def(fft_size) - kFftTable);
}

// Common continual pilot, absolute carrier index: CP32 for 32K, and for
// 16K / 8K the ceiling of every 2nd / 4th CP32 entry halved / quartered
// (Section 8.1.4.1; reproduces Tables D.1.2 and D.1.3).
std::vector<size_t> common_cp_absolute(size_t fft_size) {
    const size_t stride = 32768 / fft_size;
    std::vector<size_t> out;
    for (size_t i = 0; i * stride < 192; ++i) {
        out.push_back((kCp32[i * stride] + stride - 1) / stride);
    }
    return out;
}

// Reference sequence r_k (Section 8.1.2): 13-bit generator with
// G(x) = 1 + x^9 + x^10 + x^12 + x^13, initial sequence 0000 0000 1101 1,
// reloaded at the start of every symbol. Values 0..12 are the initial
// sequence read from x13 back to x1; later values follow the recurrence
// s[n] = s[n-9] ^ s[n-10] ^ s[n-12] ^ s[n-13].
std::vector<uint8_t> reference_sequence(size_t length) {
    static constexpr uint8_t kInitial[13] = {1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> s(length);
    for (size_t n = 0; n < length; ++n) {
        s[n] = (n < 13) ? kInitial[n] : (s[n - 9] ^ s[n - 10] ^ s[n - 12] ^ s[n - 13]);
    }
    return s;
}

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
    const size_t idx = static_cast<size_t>(config_.pattern);
    if (idx >= kNumPatterns) {
        throw std::invalid_argument("Invalid pilot pattern");
    }
    const FftDef& fft = fft_def(config_.fft_size);
    if (!kAdditional[idx][fft_index(config_.fft_size)].allowed) {
        throw std::invalid_argument("Pilot pattern not allowed for this FFT size");
    }

    if (config_.num_active_carriers == 0) {
        config_.num_active_carriers = fft.noc_max;
    }
    if (config_.num_active_carriers > fft.noc_max ||
        (fft.noc_max - config_.num_active_carriers) % fft.c_unit != 0 ||
        (fft.noc_max - config_.num_active_carriers) / fft.c_unit > kMaxReduction) {
        throw std::invalid_argument("Invalid number of active carriers for pilot extraction");
    }

    params_.pattern = config_.pattern;
    params_.dx = kPatternTable[idx].dx;
    params_.dy = kPatternTable[idx].dy;
    params_.has_edge_pilots = true;
    params_.reduced_carriers = (fft.noc_max - config_.num_active_carriers) / fft.c_unit;

    if (config_.first_active_carrier == 0) {
        config_.first_active_carrier = (config_.fft_size - config_.num_active_carriers) / 2;
    }
    params_.first_active = config_.first_active_carrier;
    params_.last_active = params_.first_active + config_.num_active_carriers - 1;
}

void PilotExtractor::generate_continual_positions() {
    const FftDef& fft = fft_def(config_.fft_size);
    const size_t cred = params_.reduced_carriers;
    const size_t noc = config_.num_active_carriers;
    const size_t shift = cred * fft.c_unit / 2;  // absolute index of relative carrier 0

    std::vector<size_t> relative;

    // Common CPs outside the configured carriers are not transmitted
    for (size_t abs_index : common_cp_absolute(config_.fft_size)) {
        if (abs_index >= shift && abs_index - shift < noc) {
            relative.push_back(abs_index - shift);
        }
    }

    for (size_t index : additional_continual_pilots(config_.fft_size, config_.pattern, cred)) {
        relative.push_back(index);
    }

    std::sort(relative.begin(), relative.end());
    relative.erase(std::unique(relative.begin(), relative.end()), relative.end());

    continual_positions_.clear();
    for (size_t rel : relative) {
        if (rel < noc) {
            continual_positions_.push_back(params_.first_active + rel);
        }
    }
    params_.num_continual_pilots = continual_positions_.size();
}

void PilotExtractor::generate_pilot_sequence() {
    const std::vector<uint8_t> r = reference_sequence(config_.num_active_carriers);
    pilot_sequence_.resize(r.size());
    for (size_t k = 0; k < r.size(); ++k) {
        // Re{c} = 2 A (1/2 - r_k): r_k = 0 is +A, r_k = 1 is -A
        pilot_sequence_[k] = r[k] ? -1 : 1;
    }
}

int8_t PilotExtractor::get_prbs_bit(size_t relative_carrier) const {
    if (relative_carrier >= pilot_sequence_.size()) {
        return 1;  // Default to +1
    }
    return pilot_sequence_[relative_carrier];
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

    // Sort by subcarrier index. Stable, so a carrier that is several
    // pilot types keeps the insertion order above, and std::unique below
    // keeps the first: SCATTERED over CONTINUAL over EDGE.
    std::stable_sort(pilots.begin(), pilots.end(), [](const PilotSymbol& a, const PilotSymbol& b) {
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
        int8_t prbs = get_prbs_bit(k - params_.first_active);
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
        int8_t prbs = get_prbs_bit(k - params_.first_active);
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
        int8_t prbs = get_prbs_bit(k - params_.first_active);
        pilot.reference_value = make_bpsk_sample(prbs);

        pilots.push_back(pilot);
    }

    return pilots;
}

const PilotPatternParams& PilotExtractor::get_pattern_params() const {
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
    int8_t prbs = get_prbs_bit(subcarrier - params_.first_active);
    return make_bpsk_sample(prbs);
}

void PilotExtractor::set_config(const PilotExtractorConfig& config) {
    config_ = config;
    init();
}

// Utility functions
std::vector<size_t> common_continual_pilots(size_t fft_size) {
    fft_def(fft_size);  // validates the size
    return common_cp_absolute(fft_size);
}

bool pilot_pattern_allowed(size_t fft_size, PilotPattern pattern) {
    const size_t idx = static_cast<size_t>(pattern);
    return idx < kNumPatterns && kAdditional[idx][fft_index(fft_size)].allowed;
}

std::vector<size_t> additional_continual_pilots(size_t fft_size, PilotPattern pattern,
                                                size_t reduced_carriers) {
    if (!pilot_pattern_allowed(fft_size, pattern)) {
        throw std::invalid_argument("Pilot pattern not allowed for this FFT size");
    }
    const AdditionalSet& set = kAdditional[static_cast<size_t>(pattern)][fft_index(fft_size)];
    std::vector<size_t> out;
    if (set.count == 0) {
        out = additional_sp32_4_8k(reduced_carriers);
    }
    for (size_t i = 0; i < set.count; ++i) {
        if (!(set.cp[i].odd_unused && (reduced_carriers % 2) == 1)) {
            out.push_back(set.cp[i].index);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

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
