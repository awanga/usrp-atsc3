// constellation_demapper.cc — ATSC 3.0 QAM symbol to soft LLR conversion
//
// Optimized demapper using direct slicer computation for uniform QAM.
// For uniform Gray-coded rectangular QAM, LLRs are computed directly from
// I/Q coordinates without iterating over constellation points.
//
// Performance: O(1) per bit for uniform QAM vs O(M) for generic NUC.
//
// Reference: ATSC A/322 Section 7.5 (Non-Uniform Constellations)

#include "constellation_demapper.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#ifdef __SSSE3__
#include <tmmintrin.h>
#endif

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace atsc3 {
namespace ofdm {

namespace {

// Gray code conversion
inline size_t to_gray(size_t n) {
    return n ^ (n >> 1);
}

inline size_t from_gray(size_t g) {
    size_t n = g;
    for (size_t mask = n >> 1; mask != 0; mask >>= 1) {
        n ^= mask;
    }
    return n;
}

// Uniform QAM normalization factors (to unit average power)
constexpr float kQamNorm4 = 1.0f / 1.4142135623730951f;     // 1/sqrt(2) for QPSK
constexpr float kQamNorm16 = 1.0f / 3.1622776601683795f;    // 1/sqrt(10)
constexpr float kQamNorm64 = 1.0f / 6.4807406984078604f;    // 1/sqrt(42)
constexpr float kQamNorm256 = 1.0f / 13.038404810405298f;   // 1/sqrt(170)
constexpr float kQamNorm1024 = 1.0f / 26.115126958080672f;  // 1/sqrt(682)
constexpr float kQamNorm4096 = 1.0f / 52.24940191045253f;   // 1/sqrt(2730)

// Inverse normalization (for slicer decision levels)
constexpr float kQamDenorm16 = 3.1622776601683795f;
constexpr float kQamDenorm64 = 6.4807406984078604f;
constexpr float kQamDenorm256 = 13.038404810405298f;
constexpr float kQamDenorm1024 = 26.115126958080672f;
constexpr float kQamDenorm4096 = 52.24940191045253f;

// ============================================================================
// OPTIMIZED HELPER FUNCTIONS
// ============================================================================

// Fast clamp-and-round without function calls
inline int8_t clamp_round(float val, float clip) {
    // Fast clamp
    if (val > clip)
        val = clip;
    else if (val < -clip)
        val = -clip;
    // Fast round (add 0.5 and truncate for positive, subtract 0.5 for negative)
    return static_cast<int8_t>(val >= 0.0f ? val + 0.5f : val - 0.5f);
}

// NUC-16 default constellation (placeholder)
const std::vector<std::pair<float, float>> kNuc16Default = {
    {0.2236f, 0.2236f},   {0.2236f, 0.6708f},   {0.6708f, 0.2236f},   {0.6708f, 0.6708f},
    {-0.2236f, 0.2236f},  {-0.2236f, 0.6708f},  {-0.6708f, 0.2236f},  {-0.6708f, 0.6708f},
    {0.2236f, -0.2236f},  {0.2236f, -0.6708f},  {0.6708f, -0.2236f},  {0.6708f, -0.6708f},
    {-0.2236f, -0.2236f}, {-0.2236f, -0.6708f}, {-0.6708f, -0.2236f}, {-0.6708f, -0.6708f}};

// Helper to build Gray-coded QAM constellation
std::vector<sample_t> build_uniform_qam(size_t bits_per_symbol, float norm) {
    size_t m = 1u << bits_per_symbol;
    size_t side = 1u << (bits_per_symbol / 2);
    std::vector<sample_t> points(m);

    for (size_t i = 0; i < m; ++i) {
        size_t i_idx = from_gray(i >> (bits_per_symbol / 2));
        size_t q_idx = from_gray(i & ((1u << (bits_per_symbol / 2)) - 1));
        float i_val = static_cast<float>(2 * static_cast<int>(i_idx) - static_cast<int>(side) + 1);
        float q_val = static_cast<float>(2 * static_cast<int>(q_idx) - static_cast<int>(side) + 1);
#ifdef ATSC3_FIXED_POINT
        points[i] = sample_t(float_to_q15(i_val * norm), float_to_q15(q_val * norm));
#else
        points[i] = sample_t(i_val * norm, q_val * norm);
#endif
    }
    return points;
}

std::vector<sample_t> build_qpsk() {
    std::vector<sample_t> points(4);
    constexpr float val = 0.7071067811865476f;
#ifdef ATSC3_FIXED_POINT
    points[0] = sample_t(float_to_q15(val), float_to_q15(val));
    points[1] = sample_t(float_to_q15(-val), float_to_q15(val));
    points[2] = sample_t(float_to_q15(-val), float_to_q15(-val));
    points[3] = sample_t(float_to_q15(val), float_to_q15(-val));
#else
    points[0] = sample_t(val, val);
    points[1] = sample_t(-val, val);
    points[2] = sample_t(-val, -val);
    points[3] = sample_t(val, -val);
#endif
    return points;
}

// ============================================================================
// FAST UNIFORM QAM SLICER-BASED LLR COMPUTATION
// ============================================================================
//
// For Gray-coded uniform QAM, the LLR for each bit depends only on the
// I or Q coordinate and can be computed directly without distance calculations.
//
// The formula for approximate LLR in uniform QAM:
//   LLR = (distance to b=1 region)² - (distance to b=0 region)²
//       = 4 * coordinate * decision_boundary / noise_variance
//
// For Gray coding, the decision boundaries form a recursive pattern.

// Compute LLR for one dimension (I or Q) of uniform QAM
// coord: Received coordinate (I or Q), already de-normalized
// half_side: Half the number of symbols per side (e.g., 4 for 64-QAM)
// bit_index: Which bit (0=MSB, counts from outer to inner boundaries)
// scale: LLR scaling factor (1/noise_variance * normalization)
inline float compute_1d_llr(float coord, int half_side, int bit_index, float scale) {
    // For Gray-coded PAM, the decision boundaries are at:
    //   bit 0 (MSB): boundary at 0
    //   bit 1: boundaries at ±half_side (e.g., ±4 for 8-PAM)
    //   bit 2: boundaries at ±half_side/2, ±3*half_side/2 (e.g., ±2, ±6)
    //   etc.
    //
    // The LLR for bit k is proportional to the distance from the nearest
    // decision boundary for that bit.

    // Compute the zone size for this bit level
    int zone = half_side >> bit_index;

    if (zone <= 0) {
        return 0.0f;
    }

    // For the MSB (bit_index=0), LLR = coord * scale (soft decision from origin)
    if (bit_index == 0) {
        // Simple soft decision: positive coord -> bit=0, negative -> bit=1
        // LLR magnitude proportional to |coord|
        return coord * scale;
    }

    // For lower bits, we need to find distance to the nearest decision boundary
    // The boundaries repeat with period 2*zone at offsets of ±zone, ±3*zone, etc.

    // Map coordinate to reduced interval [0, 4*zone) using symmetry
    float abs_coord = std::fabs(coord);

    // The pattern repeats every 4*zone (two zones for bit=0, two for bit=1)
    float period = 4.0f * zone;
    float reduced = std::fmod(abs_coord, period);
    if (reduced < 0)
        reduced += period;

    // Within one period [0, 4*zone):
    //   [0, zone): bit=0, distance to boundary = zone - reduced
    //   [zone, 2*zone): bit=1, distance to boundary = reduced - zone
    //   [2*zone, 3*zone): bit=0, distance to boundary = reduced - 2*zone
    //   [3*zone, 4*zone): bit=1, distance to boundary = 4*zone - reduced

    float llr;
    if (reduced < zone) {
        // Region bit=0, nearest boundary is at zone
        llr = (zone - reduced) * scale;
    } else if (reduced < 2 * zone) {
        // Region bit=1, nearest boundary is at zone
        llr = -(reduced - zone) * scale;
    } else if (reduced < 3 * zone) {
        // Region bit=0, nearest boundary is at 2*zone or 3*zone
        float d0 = reduced - 2 * zone;
        float d1 = 3 * zone - reduced;
        llr = std::min(d0, d1) * scale;
    } else {
        // Region bit=1, nearest boundary is at 3*zone
        llr = -(4 * zone - reduced) * scale;
    }

    return llr;
}

// Fast QPSK demapping (2 bits) - optimized
inline void demap_qpsk_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // QPSK: bit 0 from I, bit 1 from Q
    float fclip = static_cast<float>(clip);
    out[0] = clamp_round(i_coord * scale, fclip);
    out[1] = clamp_round(q_coord * scale, fclip);
}

// Fast 16-QAM demapping (4 bits) - optimized
inline void demap_qam16_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 16-QAM: 4x4 grid, constellation at ±1, ±3 (normalized)
    float fclip = static_cast<float>(clip);

    // Denormalize to grid coordinates
    float i_dn = i_coord * kQamDenorm16;
    float q_dn = q_coord * kQamDenorm16;
    float s = scale / kQamDenorm16;

    // Absolute values
    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // Bit 0 (I MSB): boundary at 0
    out[0] = clamp_round(i_dn * s, fclip);

    // Bit 1 (Q MSB): boundary at 0
    out[1] = clamp_round(q_dn * s, fclip);

    // Bit 2 (I LSB): boundary at ±2
    out[2] = clamp_round((2.0f - abs_i) * s, fclip);

    // Bit 3 (Q LSB): boundary at ±2
    out[3] = clamp_round((2.0f - abs_q) * s, fclip);
}

// Fast 64-QAM demapping (6 bits) - fully optimized
inline void demap_qam64_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 64-QAM: 8x8 grid, constellation at ±1, ±3, ±5, ±7 (normalized)
    // I component -> bits 0, 2, 4
    // Q component -> bits 1, 3, 5
    //
    // For Gray-coded 64-QAM:
    // Bit 0: sign of I (MSB)
    // Bit 1: sign of Q
    // Bit 2: |I| < 4 vs >= 4 (middle boundary)
    // Bit 3: |Q| < 4 vs >= 4
    // Bit 4: |I| mod 4 < 2 vs >= 2 (inner boundaries at ±2, ±6)
    // Bit 5: |Q| mod 4 < 2 vs >= 2

    float fclip = static_cast<float>(clip);

    // Denormalize to grid coordinates (±1, ±3, ±5, ±7)
    float i_dn = i_coord * kQamDenorm64;
    float q_dn = q_coord * kQamDenorm64;

    // Combined scale factor
    float s = scale / kQamDenorm64;

    // Absolute values (branchless: clear sign bit)
    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // Bit 0 (I MSB): LLR = distance from origin boundary
    // Positive I -> bit=0 more likely -> positive LLR
    out[0] = clamp_round(i_dn * s, fclip);

    // Bit 1 (Q MSB): same logic for Q
    out[1] = clamp_round(q_dn * s, fclip);

    // Bit 2 (I): boundary at ±4
    // |I| < 4 -> bit=0; |I| >= 4 -> bit=1
    // LLR = (4 - |I|) * scale
    out[2] = clamp_round((4.0f - abs_i) * s, fclip);

    // Bit 3 (Q): boundary at ±4
    out[3] = clamp_round((4.0f - abs_q) * s, fclip);

    // Bit 4 (I): boundaries at ±2, ±6 (repeats every 4)
    // Reduce |I| to [0, 4) using fast modulo
    float red_i = abs_i - 4.0f * static_cast<int>(abs_i * 0.25f);  // abs_i mod 4
    // Within [0,4): boundary at 2
    out[4] = clamp_round((2.0f - red_i) * s, fclip);

    // Bit 5 (Q): boundaries at ±2, ±6
    float red_q = abs_q - 4.0f * static_cast<int>(abs_q * 0.25f);
    out[5] = clamp_round((2.0f - red_q) * s, fclip);
}

// Fast 256-QAM demapping (8 bits) - optimized
inline void demap_qam256_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 256-QAM: 16x16 grid
    float fclip = static_cast<float>(clip);

    float i_dn = i_coord * kQamDenorm256;
    float q_dn = q_coord * kQamDenorm256;
    float s = scale / kQamDenorm256;

    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // I bits: 0, 2, 4, 6
    out[0] = clamp_round(i_dn * s, fclip);
    out[2] = clamp_round((8.0f - abs_i) * s, fclip);
    float red_i4 = abs_i - 8.0f * static_cast<int>(abs_i * 0.125f);  // mod 8
    out[4] = clamp_round((4.0f - red_i4) * s, fclip);
    float red_i6 = abs_i - 4.0f * static_cast<int>(abs_i * 0.25f);  // mod 4
    out[6] = clamp_round((2.0f - red_i6) * s, fclip);

    // Q bits: 1, 3, 5, 7
    out[1] = clamp_round(q_dn * s, fclip);
    out[3] = clamp_round((8.0f - abs_q) * s, fclip);
    float red_q5 = abs_q - 8.0f * static_cast<int>(abs_q * 0.125f);
    out[5] = clamp_round((4.0f - red_q5) * s, fclip);
    float red_q7 = abs_q - 4.0f * static_cast<int>(abs_q * 0.25f);
    out[7] = clamp_round((2.0f - red_q7) * s, fclip);
}

}  // namespace

ConstellationDemapper::ConstellationDemapper(const DemapperConfig& config) : config_(config) {
    init();
}

ConstellationDemapper::~ConstellationDemapper() = default;

void ConstellationDemapper::init() {
    bits_per_symbol_ = get_bits_per_symbol(config_.modulation);

    if (is_nuc_modulation(config_.modulation)) {
        generate_nuc_constellation();
    } else {
        generate_uniform_constellation();
    }

    build_bit_mappings();
    llr_scale_ = 1.0f / config_.noise_variance;
}

void ConstellationDemapper::generate_uniform_constellation() {
    switch (config_.modulation) {
        case config::Modulation::QPSK:
            constellation_points_ = build_qpsk();
            break;
        case config::Modulation::QAM16:
            constellation_points_ = build_uniform_qam(4, kQamNorm16);
            break;
        case config::Modulation::QAM64:
            constellation_points_ = build_uniform_qam(6, kQamNorm64);
            break;
        case config::Modulation::QAM256:
            constellation_points_ = build_uniform_qam(8, kQamNorm256);
            break;
        case config::Modulation::QAM1024:
            constellation_points_ = build_uniform_qam(10, kQamNorm1024);
            break;
        case config::Modulation::QAM4096:
            constellation_points_ = build_uniform_qam(12, kQamNorm4096);
            break;
        default:
            throw std::invalid_argument("Unsupported uniform modulation type");
    }
}

void ConstellationDemapper::generate_nuc_constellation() {
    size_t m = get_constellation_size(config_.modulation);
    constellation_points_.resize(m);

    switch (config_.modulation) {
        case config::Modulation::NUC_QPSK:
            constellation_points_ = build_qpsk();
            break;
        case config::Modulation::NUC_16:
            if (kNuc16Default.size() != m) {
                throw std::runtime_error("NUC-16 table size mismatch");
            }
            for (size_t i = 0; i < m; ++i) {
#ifdef ATSC3_FIXED_POINT
                constellation_points_[i] = sample_t(float_to_q15(kNuc16Default[i].first),
                                                    float_to_q15(kNuc16Default[i].second));
#else
                constellation_points_[i] =
                    sample_t(kNuc16Default[i].first, kNuc16Default[i].second);
#endif
            }
            break;
        case config::Modulation::NUC_64:
            constellation_points_ = build_uniform_qam(6, kQamNorm64);
            break;
        case config::Modulation::NUC_256:
            constellation_points_ = build_uniform_qam(8, kQamNorm256);
            break;
        case config::Modulation::NUC_1024:
            constellation_points_ = build_uniform_qam(10, kQamNorm1024);
            break;
        case config::Modulation::NUC_4096:
            constellation_points_ = build_uniform_qam(12, kQamNorm4096);
            break;
        default:
            throw std::invalid_argument("Unsupported NUC modulation type");
    }
}

void ConstellationDemapper::build_bit_mappings() {
    size_t m = constellation_points_.size();
    bit_sets_.resize(bits_per_symbol_);
    bit_clears_.resize(bits_per_symbol_);

    for (size_t b = 0; b < bits_per_symbol_; ++b) {
        bit_sets_[b].clear();
        bit_clears_[b].clear();
        bit_sets_[b].reserve(m / 2);
        bit_clears_[b].reserve(m / 2);

        size_t bit_mask = 1u << (bits_per_symbol_ - 1 - b);
        for (size_t i = 0; i < m; ++i) {
            if (i & bit_mask) {
                bit_sets_[b].push_back(i);
            } else {
                bit_clears_[b].push_back(i);
            }
        }
    }
}

float ConstellationDemapper::squared_distance(sample_t a, sample_t b) const {
#ifdef ATSC3_FIXED_POINT
    float a_re = q15_to_float(a.real());
    float a_im = q15_to_float(a.imag());
    float b_re = q15_to_float(b.real());
    float b_im = q15_to_float(b.imag());
#else
    float a_re = a.real();
    float a_im = a.imag();
    float b_re = b.real();
    float b_im = b.imag();
#endif
    float d_re = a_re - b_re;
    float d_im = a_im - b_im;
    return d_re * d_re + d_im * d_im;
}

int8_t ConstellationDemapper::compute_llr_max_log(sample_t symbol, size_t bit_index) const {
    // Generic max-log LLR for NUC constellations (slower path)
    float min_dist_0 = std::numeric_limits<float>::max();
    float min_dist_1 = std::numeric_limits<float>::max();

    for (size_t idx : bit_clears_[bit_index]) {
        float dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_0)
            min_dist_0 = dist;
    }

    for (size_t idx : bit_sets_[bit_index]) {
        float dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_1)
            min_dist_1 = dist;
    }

    float llr = (min_dist_1 - min_dist_0) * llr_scale_;
    float clip = static_cast<float>(config_.llr_clip);
    llr = std::max(-clip, std::min(clip, llr));
    return static_cast<int8_t>(std::round(llr));
}

void ConstellationDemapper::demap_symbol(sample_t symbol, int8_t* llr_out) const {
    // Extract I/Q coordinates
#ifdef ATSC3_FIXED_POINT
    float i_coord = q15_to_float(symbol.real());
    float q_coord = q15_to_float(symbol.imag());
#else
    float i_coord = symbol.real();
    float q_coord = symbol.imag();
#endif

    // Use fast path for uniform QAM
    if (!is_nuc_modulation(config_.modulation)) {
        switch (config_.modulation) {
            case config::Modulation::QPSK:
                demap_qpsk_fast(i_coord, q_coord, llr_scale_, config_.llr_clip, llr_out);
                return;
            case config::Modulation::QAM16:
                demap_qam16_fast(i_coord, q_coord, llr_scale_, config_.llr_clip, llr_out);
                return;
            case config::Modulation::QAM64:
                demap_qam64_fast(i_coord, q_coord, llr_scale_, config_.llr_clip, llr_out);
                return;
            case config::Modulation::QAM256:
                demap_qam256_fast(i_coord, q_coord, llr_scale_, config_.llr_clip, llr_out);
                return;
            default:
                break;
        }
    }

    // Fallback to generic max-log for NUC and higher-order QAM
    for (size_t b = 0; b < bits_per_symbol_; ++b) {
        llr_out[b] = compute_llr_max_log(symbol, b);
    }
}

DemapResult ConstellationDemapper::demap(const sample_t* symbols, size_t num_symbols) const {
    DemapResult result;
    if (symbols == nullptr || num_symbols == 0) {
        return result;
    }

    size_t total_bits = num_symbols * bits_per_symbol_;
    result.llr.resize(total_bits);
    result.num_symbols = demap(symbols, num_symbols, result.llr.data()) / bits_per_symbol_;
    result.bits_per_symbol = bits_per_symbol_;
    result.is_valid = true;
    return result;
}

size_t ConstellationDemapper::demap(const sample_t* symbols, size_t num_symbols,
                                    int8_t* llr_out) const {
    if (symbols == nullptr || llr_out == nullptr || num_symbols == 0) {
        return 0;
    }

    int8_t* out = llr_out;

    // Use optimized batch processing for uniform QAM
    if (!is_nuc_modulation(config_.modulation)) {
        switch (config_.modulation) {
            case config::Modulation::QPSK:
                for (size_t i = 0; i < num_symbols; ++i) {
#ifdef ATSC3_FIXED_POINT
                    float i_c = q15_to_float(symbols[i].real());
                    float q_c = q15_to_float(symbols[i].imag());
#else
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
#endif
                    demap_qpsk_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 2;
                }
                return num_symbols * 2;

            case config::Modulation::QAM16:
                for (size_t i = 0; i < num_symbols; ++i) {
#ifdef ATSC3_FIXED_POINT
                    float i_c = q15_to_float(symbols[i].real());
                    float q_c = q15_to_float(symbols[i].imag());
#else
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
#endif
                    demap_qam16_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 4;
                }
                return num_symbols * 4;

            case config::Modulation::QAM64:
                for (size_t i = 0; i < num_symbols; ++i) {
#ifdef ATSC3_FIXED_POINT
                    float i_c = q15_to_float(symbols[i].real());
                    float q_c = q15_to_float(symbols[i].imag());
#else
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
#endif
                    demap_qam64_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 6;
                }
                return num_symbols * 6;

            case config::Modulation::QAM256:
                for (size_t i = 0; i < num_symbols; ++i) {
#ifdef ATSC3_FIXED_POINT
                    float i_c = q15_to_float(symbols[i].real());
                    float q_c = q15_to_float(symbols[i].imag());
#else
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
#endif
                    demap_qam256_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 8;
                }
                return num_symbols * 8;

            default:
                break;
        }
    }

    // Fallback for NUC and 1024/4096-QAM
    for (size_t i = 0; i < num_symbols; ++i) {
        demap_symbol(symbols[i], out);
        out += bits_per_symbol_;
    }
    return num_symbols * bits_per_symbol_;
}

void ConstellationDemapper::set_config(const DemapperConfig& config) {
    config_ = config;
    init();
}

void ConstellationDemapper::reset() {
    // No state to reset
}

}  // namespace ofdm
}  // namespace atsc3
