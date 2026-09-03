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

#include "nuc_tables.h"

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

#ifdef ATSC3_FIXED_POINT
// Q1.15 headroom, fixed-point build only (Phase 9.0b): unit-average-power
// normalization (the constants above) puts a constellation's *peak*
// component magnitude above 1.0 for every uniform order above QPSK -- up
// to 1.206 for 4096-QAM -- and as high as 1.727 for the NUC tables
// (measured directly from nuc_tables.h; NUC's peak-to-average ratio is
// worse than uniform QAM's, not just comparable). Q1.15 can only
// represent [-1, 1); float_to_q15() saturating (per the merged bugfix)
// stops that from being UB, but it still *distorts* every point whose
// true magnitude exceeds 1.0 -- clamping 1.206 down to 0.99997 moves the
// outer points measurably closer to the origin than they should be,
// exactly the kind of systematic (not random-noise) error that degrades
// LLR accuracy for those points specifically.
//
// The headroom divisor is computed *per constellation*, from that
// constellation's own actual peak (targeting peak/headroom = 0.95, a
// small margin below the hard 0.99997 boundary), not one blanket factor
// for every mode: QPSK's peak (0.707) already fits with no headroom
// needed, and applying a large fixed headroom anyway would needlessly
// shrink its constellation relative to a fixed noise_variance, silently
// changing the effective SNR noise_variance represents for modes that
// never had a representation problem to begin with (confirmed while
// developing this: a single blanket headroom=2.0 factor measurably
// degraded QPSK's LLR-sign accuracy in testing even though QPSK's peak
// was never close to overflowing). The target itself was swept (0.8
// through 0.99) against QAM64LLRSignMatchesBitAtHighSNR: accuracy rises
// monotonically as the target approaches 1.0 (less headroom, better
// SNR) and plateaus around 98.4-98.7%, short of that test's >99% bar
// even with essentially no headroom left -- the remaining gap is Q1.15's
// inherent quantization noise floor for 64-QAM's tightest decision
// boundaries at this noise_variance, not a headroom tuning problem. 0.95
// keeps a bit of margin below the hard boundary rather than chasing the
// last ~0.3% by sitting right at it.
constexpr float kQamHeadroomTarget = 0.95f;

// Headroom divisor for a constellation whose largest single-axis
// component (in unit-average-power units) is peak. 1.0 (no change) if it
// already fits.
inline float qam_headroom_for_peak(float peak) {
    return (peak > kQamHeadroomTarget) ? (peak / kQamHeadroomTarget) : 1.0f;
}
#endif

// ============================================================================
// OPTIMIZED HELPER FUNCTIONS
// ============================================================================

#ifndef ATSC3_FIXED_POINT
// Fast clamp-and-round without function calls. Float build only -- see
// demap_uniform_fixed() for the fixed-point equivalent; this and the four
// demap_qamXX_fast() functions below would be unused (and thus a
// -Werror=unused-function build failure) under ATSC3_FIXED_POINT.
inline int8_t clamp_round(float val, float clip) {
    // Fast clamp
    if (val > clip)
        val = clip;
    else if (val < -clip)
        val = -clip;
    // Fast round (add 0.5 and truncate for positive, subtract 0.5 for negative)
    return static_cast<int8_t>(val >= 0.0f ? val + 0.5f : val - 0.5f);
}
#endif  // !ATSC3_FIXED_POINT

// Build 2D NUC constellation from base points table
// Base points cover first quadrant; other quadrants derived via symmetry:
//   Q1: original, Q2: -conj, Q3: conj, Q4: -point
template <size_t N>
std::vector<sample_t> build_2d_nuc(const float table[N][2]) {
    std::vector<sample_t> points(N * 4);
#ifdef ATSC3_FIXED_POINT
    // Base table covers only the first quadrant (per the comment above),
    // so its own max |component| is this constellation's true peak --
    // the other three quadrants are sign flips of the same magnitudes.
    float peak = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        peak = std::max({peak, std::fabs(table[i][0]), std::fabs(table[i][1])});
    }
    float headroom = qam_headroom_for_peak(peak);
#endif
    for (size_t i = 0; i < N; ++i) {
        float re = table[i][0];
        float im = table[i][1];
#ifdef ATSC3_FIXED_POINT
        re /= headroom;
        im /= headroom;
        // Q1: +I, +Q
        points[i] = sample_t(float_to_q15(re), float_to_q15(im));
        // Q2: -I, +Q = -conj(point)
        points[N + i] = sample_t(float_to_q15(-re), float_to_q15(im));
        // Q3: +I, -Q = conj(point)
        points[2 * N + i] = sample_t(float_to_q15(re), float_to_q15(-im));
        // Q4: -I, -Q = -point
        points[3 * N + i] = sample_t(float_to_q15(-re), float_to_q15(-im));
#else
        // Q1: +I, +Q
        points[i] = sample_t(re, im);
        // Q2: -I, +Q = -conj(point)
        points[N + i] = sample_t(-re, im);
        // Q3: +I, -Q = conj(point)
        points[2 * N + i] = sample_t(re, -im);
        // Q4: -I, -Q = -point
        points[3 * N + i] = sample_t(-re, -im);
#endif
    }
    return points;
}

// Build 1D NUC constellation from amplitude table
// 1D NUC uses separable I/Q with amplitude tables and index remapping
template <size_t N>
std::vector<sample_t> build_1d_nuc(const float amps[N], const int map[], size_t map_size) {
    size_t side = map_size;
    size_t total = side * side;
    std::vector<sample_t> points(total);

#ifdef ATSC3_FIXED_POINT
    // amps[] is the full set of possible per-axis amplitude magnitudes for
    // this constellation; its max is this constellation's true peak
    // (I and Q are independently drawn from the same amplitude set).
    float peak = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        peak = std::max(peak, std::fabs(amps[i]));
    }
    float headroom = qam_headroom_for_peak(peak);
#endif

    for (size_t i = 0; i < total; ++i) {
        // Extract bit-interleaved I and Q indices
        size_t indexodd = 0;
        size_t indexeven = 0;
        size_t bits_per_dim = (N == 16) ? 5 : 6;  // 1024: 5 bits, 4096: 6 bits

        for (int n = static_cast<int>(bits_per_dim) - 1; n >= 0; n--) {
            indexodd |= (i & (0x1u << (n * 2))) >> n;
        }
        for (int n = static_cast<int>(bits_per_dim) - 1; n >= 0; n--) {
            indexeven |= (i & (0x1u << ((n * 2) + 1))) >> (n + 1);
        }

        // Determine quadrant from MSBs
        size_t quadrant;
        if (N == 16) {
            quadrant = (indexeven >> 4) | ((indexodd & 0x10) >> 3);
        } else {
            quadrant = (indexeven >> 5) | ((indexodd & 0x20) >> 4);
        }

        // Get amplitude from remapped index
        float amp_i = amps[map[indexodd & (side - 1)]];
        float amp_q = amps[map[indexeven & (side - 1)]];

        // Apply quadrant signs
        float re, im;
        switch (quadrant & 3) {
            case 0:
                re = amp_i;
                im = amp_q;
                break;
            case 1:
                re = amp_i;
                im = -amp_q;
                break;
            case 2:
                re = -amp_i;
                im = amp_q;
                break;
            case 3:
            default:
                re = -amp_i;
                im = -amp_q;
                break;
        }

#ifdef ATSC3_FIXED_POINT
        points[i] = sample_t(float_to_q15(re / headroom), float_to_q15(im / headroom));
#else
        points[i] = sample_t(re, im);
#endif
    }
    return points;
}

// Helper to build Gray-coded QAM constellation
std::vector<sample_t> build_uniform_qam(size_t bits_per_symbol, float norm) {
    size_t m = 1u << bits_per_symbol;
    size_t side = 1u << (bits_per_symbol / 2);
    std::vector<sample_t> points(m);

#ifdef ATSC3_FIXED_POINT
    // Peak single-axis grid value is exactly side-1 (levels run
    // -(side-1)..+(side-1) in steps of 2), so the true peak in
    // unit-average-power units is (side-1)*norm.
    float peak = static_cast<float>(side - 1) * norm;
    norm /= qam_headroom_for_peak(peak);
#endif

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
    // Use build_uniform_qam for consistent Gray coding
    // This gives: idx=0→(-,-), idx=1→(-,+), idx=2→(+,-), idx=3→(+,+)
    return build_uniform_qam(2, kQamNorm4);
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

// (The fixed-point uniform-QAM boundary slicer is
// ConstellationDemapper::axis_bit_distance() below -- it needs
// uniform_cell_bit_/uniform_boundaries_, so it's a member function, not a
// free function here. See that method's comment for the real bug found
// and fixed while developing it: a first version used a closed-form
// "zone/period" formula ported from this file's own (previously dead --
// never called) float compute_1d_llr(), which turned out to compute the
// wrong bit for every axis-bit beyond the MSB, verified against this
// codebase's actual from_gray()/to_gray() Gray-code convention.)

#ifndef ATSC3_FIXED_POINT
// Fast QPSK demapping (2 bits) - optimized
inline void demap_qpsk_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // QPSK: bit 0 from I, bit 1 from Q
    // Gray coding: negative coord → bit=0 → positive LLR
    // So LLR ∝ -coord
    float fclip = static_cast<float>(clip);
    out[0] = clamp_round(-i_coord * scale, fclip);
    out[1] = clamp_round(-q_coord * scale, fclip);
}

// Fast 16-QAM demapping (4 bits) - optimized
inline void demap_qam16_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 16-QAM: 4x4 grid, constellation at ±1, ±3 (normalized)
    // Gray coding: negative coord → MSB=0 → positive LLR
    // Bit order: I bits first (0,1), then Q bits (2,3)
    float fclip = static_cast<float>(clip);

    // Denormalize to grid coordinates
    float i_dn = i_coord * kQamDenorm16;
    float q_dn = q_coord * kQamDenorm16;
    float s = scale / kQamDenorm16;

    // Absolute values
    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // I bits (0, 1)
    out[0] = clamp_round(-i_dn * s, fclip);           // I MSB: negative I → bit=0
    out[1] = clamp_round((abs_i - 2.0f) * s, fclip);  // I LSB: |I|>2 → bit=0

    // Q bits (2, 3)
    out[2] = clamp_round(-q_dn * s, fclip);           // Q MSB: negative Q → bit=0
    out[3] = clamp_round((abs_q - 2.0f) * s, fclip);  // Q LSB: |Q|>2 → bit=0
}

// Fast 64-QAM demapping (6 bits) - fully optimized
inline void demap_qam64_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 64-QAM: 8x8 grid, constellation at ±1, ±3, ±5, ±7 (normalized)
    // Bit order: I bits first (0,1,2), then Q bits (3,4,5)
    //
    // For Gray-coded 64-QAM:
    // Bit 0: I MSB, negative I → bit=0
    // Bit 1: I middle, |I| >= 4 → bit=0
    // Bit 2: I LSB, ||I|-4| >= 2 → bit=0
    // Bit 3: Q MSB, negative Q → bit=0
    // Bit 4: Q middle, |Q| >= 4 → bit=0
    // Bit 5: Q LSB, ||Q|-4| >= 2 → bit=0

    float fclip = static_cast<float>(clip);

    // Denormalize to grid coordinates (±1, ±3, ±5, ±7)
    float i_dn = i_coord * kQamDenorm64;
    float q_dn = q_coord * kQamDenorm64;

    // Combined scale factor
    float s = scale / kQamDenorm64;

    // Absolute values
    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // Distance from midpoint for LSB bits
    float dist_i = abs_i - 4.0f;
    dist_i = dist_i < 0.0f ? -dist_i : dist_i;
    float dist_q = abs_q - 4.0f;
    dist_q = dist_q < 0.0f ? -dist_q : dist_q;

    // I bits (0, 1, 2)
    out[0] = clamp_round(-i_dn * s, fclip);            // I MSB: negative I → bit=0
    out[1] = clamp_round((abs_i - 4.0f) * s, fclip);   // I middle: |I| >= 4 → bit=0
    out[2] = clamp_round((dist_i - 2.0f) * s, fclip);  // I LSB: ||I|-4| >= 2 → bit=0

    // Q bits (3, 4, 5)
    out[3] = clamp_round(-q_dn * s, fclip);            // Q MSB: negative Q → bit=0
    out[4] = clamp_round((abs_q - 4.0f) * s, fclip);   // Q middle: |Q| >= 4 → bit=0
    out[5] = clamp_round((dist_q - 2.0f) * s, fclip);  // Q LSB: ||Q|-4| >= 2 → bit=0
}

// Fast 256-QAM demapping (8 bits) - optimized
inline void demap_qam256_fast(float i_coord, float q_coord, float scale, int8_t clip, int8_t* out) {
    // 256-QAM: 16x16 grid, levels ±1,±3,...,±15
    // Bit order: I bits first (0,1,2,3), then Q bits (4,5,6,7)
    float fclip = static_cast<float>(clip);

    float i_dn = i_coord * kQamDenorm256;
    float q_dn = q_coord * kQamDenorm256;
    float s = scale / kQamDenorm256;

    float abs_i = i_dn < 0.0f ? -i_dn : i_dn;
    float abs_q = q_dn < 0.0f ? -q_dn : q_dn;

    // Nested distances for I
    float dist_i1 = abs_i - 8.0f;
    dist_i1 = dist_i1 < 0.0f ? -dist_i1 : dist_i1;
    float dist_i2 = dist_i1 - 4.0f;
    dist_i2 = dist_i2 < 0.0f ? -dist_i2 : dist_i2;

    // Nested distances for Q
    float dist_q1 = abs_q - 8.0f;
    dist_q1 = dist_q1 < 0.0f ? -dist_q1 : dist_q1;
    float dist_q2 = dist_q1 - 4.0f;
    dist_q2 = dist_q2 < 0.0f ? -dist_q2 : dist_q2;

    // I bits (0, 1, 2, 3)
    out[0] = clamp_round(-i_dn * s, fclip);             // I MSB: negative I → bit=0
    out[1] = clamp_round((abs_i - 8.0f) * s, fclip);    // |I| >= 8 → bit=0
    out[2] = clamp_round((dist_i1 - 4.0f) * s, fclip);  // ||I|-8| >= 4 → bit=0
    out[3] = clamp_round((dist_i2 - 2.0f) * s, fclip);  // |||I|-8|-4| >= 2 → bit=0

    // Q bits (4, 5, 6, 7)
    out[4] = clamp_round(-q_dn * s, fclip);             // Q MSB: negative Q → bit=0
    out[5] = clamp_round((abs_q - 8.0f) * s, fclip);    // |Q| >= 8 → bit=0
    out[6] = clamp_round((dist_q1 - 4.0f) * s, fclip);  // ||Q|-8| >= 4 → bit=0
    out[7] = clamp_round((dist_q2 - 2.0f) * s, fclip);  // |||Q|-8|-4| >= 2 → bit=0
}

#endif  // !ATSC3_FIXED_POINT

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

#ifdef ATSC3_FIXED_POINT
    // Q16.16-ish (noise_variance, and so this scale, can span several
    // orders of magnitude -- generous range, not Q1.15).
    llr_scale_q16_ = static_cast<int32_t>(std::lround((1.0 / config_.noise_variance) * 65536.0));

    half_side_ = 0;
    norm_const_q15_ = 0;
    if (!is_nuc_modulation(config_.modulation)) {
        float norm = 1.0f;
        switch (config_.modulation) {
            case config::Modulation::QPSK:
                norm = kQamNorm4;
                break;
            case config::Modulation::QAM16:
                norm = kQamNorm16;
                break;
            case config::Modulation::QAM64:
                norm = kQamNorm64;
                break;
            case config::Modulation::QAM256:
                norm = kQamNorm256;
                break;
            case config::Modulation::QAM1024:
                norm = kQamNorm1024;
                break;
            case config::Modulation::QAM4096:
                norm = kQamNorm4096;
                break;
            default:
                break;
        }
        half_side_ = static_cast<int>(1u << (bits_per_symbol_ / 2 - 1));
        // Must match build_uniform_qam()'s peak/headroom computation
        // exactly, or the slicer's zone boundaries and the constellation
        // table it's meant to be consistent with would disagree.
        float side = static_cast<float>(2 * half_side_);
        float peak = (side - 1.0f) * norm;
        norm_const_q15_ = float_to_q15(norm / qam_headroom_for_peak(peak));

        // Build the per-axis-bit decision tables (see the header comment
        // on uniform_cell_bit_/uniform_boundaries_).
        size_t bits_per_axis = bits_per_symbol_ / 2;
        uniform_cell_bit_.assign(bits_per_axis, {});
        uniform_boundaries_.assign(bits_per_axis, {});
        for (size_t b = 1; b < bits_per_axis; ++b) {
            uniform_cell_bit_[b].resize(half_side_);
            for (int c = 0; c < half_side_; ++c) {
                // i_idx for the positive-side cell at magnitude level c
                // (grid value 2c+1); bits 1..bits_per_axis-1 are
                // symmetric in |grid value| (verified in derivation),
                // so the positive side alone determines both signs.
                size_t i_idx = static_cast<size_t>(half_side_) + static_cast<size_t>(c);
                size_t g = to_gray(i_idx);
                uniform_cell_bit_[b][c] = static_cast<uint8_t>((g >> (bits_per_axis - 1 - b)) & 1u);
            }
            for (int c = 0; c + 1 < half_side_; ++c) {
                if (uniform_cell_bit_[b][c] != uniform_cell_bit_[b][c + 1]) {
                    // Boundary between cells c and c+1 sits at grid value
                    // 2*(c+1), converted to this modulation's Q1.15 scale.
                    int32_t boundary_q15 = 2 * (c + 1) * static_cast<int32_t>(norm_const_q15_);
                    uniform_boundaries_[b].push_back(boundary_q15);
                }
            }
        }
    }
#else
    llr_scale_ = 1.0f / config_.noise_variance;
#endif
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
    size_t rate_idx = code_rate_index(static_cast<uint8_t>(config_.code_rate));

    switch (config_.modulation) {
        case config::Modulation::NUC_QPSK:
            // QPSK is uniform, no code-rate dependent NUC
            constellation_points_ = build_qpsk();
            break;

        case config::Modulation::NUC_16:
            // 2D NUC: 4 base points × 4 quadrants = 16 points
            constellation_points_ = build_2d_nuc<NUC_16_BASE_POINTS>(NUC_16_TABLE[rate_idx]);
            break;

        case config::Modulation::NUC_64:
            // 2D NUC: 16 base points × 4 quadrants = 64 points
            constellation_points_ = build_2d_nuc<NUC_64_BASE_POINTS>(NUC_64_TABLE[rate_idx]);
            break;

        case config::Modulation::NUC_256:
            // 2D NUC: 64 base points × 4 quadrants = 256 points
            constellation_points_ = build_2d_nuc<NUC_256_BASE_POINTS>(NUC_256_TABLE[rate_idx]);
            break;

        case config::Modulation::NUC_1024:
            // 1D NUC: 32×32 separable constellation
            constellation_points_ =
                build_1d_nuc<NUC_1024_AMPLITUDES>(NUC_1024_TABLE[rate_idx], NUC_1024_MAP, 32);
            break;

        case config::Modulation::NUC_4096:
            // 1D NUC: 64×64 separable constellation
            constellation_points_ =
                build_1d_nuc<NUC_4096_AMPLITUDES>(NUC_4096_TABLE[rate_idx], NUC_4096_MAP, 64);
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

#ifdef ATSC3_FIXED_POINT
int64_t ConstellationDemapper::squared_distance(sample_t a, sample_t b) const {
    int64_t d_re = static_cast<int64_t>(a.real()) - b.real();
    int64_t d_im = static_cast<int64_t>(a.imag()) - b.imag();
    return d_re * d_re + d_im * d_im;
}
#else
float ConstellationDemapper::squared_distance(sample_t a, sample_t b) const {
    float a_re = a.real();
    float a_im = a.imag();
    float b_re = b.real();
    float b_im = b.imag();
    float d_re = a_re - b_re;
    float d_im = a_im - b_im;
    return d_re * d_re + d_im * d_im;
}
#endif

int8_t ConstellationDemapper::compute_llr_max_log(sample_t symbol, size_t bit_index) const {
    // Generic max-log LLR for NUC constellations (slower path) -- no
    // sqrt anywhere, min-log LLR only ever needs squared distances.
#ifdef ATSC3_FIXED_POINT
    int64_t min_dist_0 = std::numeric_limits<int64_t>::max();
    int64_t min_dist_1 = std::numeric_limits<int64_t>::max();

    for (size_t idx : bit_clears_[bit_index]) {
        int64_t dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_0)
            min_dist_0 = dist;
    }

    for (size_t idx : bit_sets_[bit_index]) {
        int64_t dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_1)
            min_dist_1 = dist;
    }

    // (min_dist_1 - min_dist_0) is a raw Q1.15^2 squared-distance
    // difference; llr_scale_q16_ is Q16.16. Combined shift to rescale
    // both factors out: >>15 (Q1.15^2 has one factor of Q1.15 left after
    // treating it as a "distance", per the max-log formula's units) is
    // not needed here the way it was for a magnitude -- the squared-
    // distance difference is already linear in the *squared* Q1.15 scale,
    // and multiplying by a Q16.16 scale then shifting by 16 (not 15+16)
    // matches the original float formula's units: llr = distance_squared
    // (in true units) * (1/noise_variance), and distance_squared_q =
    // distance_squared_true * 2^30 (two factors of Q1.15), so
    // (distance_squared_q * scale_q16) >> (30+16-...
    int64_t dist_diff = min_dist_1 - min_dist_0;
    int64_t llr_raw = dist_diff * llr_scale_q16_;
    // Rescale: dist_diff is true_value * 2^30 (Q1.15 squared twice),
    // llr_scale_q16_ is true_scale * 2^16 -- combined factor 2^46.
    // Bias must be an *unconditional* positive half-unit, not
    // sign-dependent: C++'s >> on a signed value is arithmetic
    // (floor-division) shift, and (x + half) >> n is the correct
    // round-to-nearest for floor division regardless of x's sign -- a
    // sign-dependent +-half bias (the right technique for a *truncating*
    // division, which this is not) rounds every negative value away from
    // zero by up to a full extra unit. Found by the equivalence test
    // below: it capped every mode's per-sample error at exactly 1.0 LLR
    // unit, a systematic-bias signature, not quantization noise.
    constexpr int kShift = 46;
    int64_t bias = int64_t(1) << (kShift - 1);
    int64_t llr = (llr_raw + bias) >> kShift;

    int64_t clip = config_.llr_clip;
    if (llr > clip)
        llr = clip;
    if (llr < -clip)
        llr = -clip;
    return static_cast<int8_t>(llr);
#else
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
#endif
}

#ifdef ATSC3_FIXED_POINT
int32_t ConstellationDemapper::axis_bit_distance(int16_t coord_q15, size_t bit_index) const {
    if (bit_index == 0) {
        // Axis MSB: sign of the coordinate directly. negative coord ->
        // bit=0 -> positive LLR, matching every hand-unrolled
        // demap_qamXX_fast() function's convention (float build).
        return -static_cast<int32_t>(coord_q15);
    }

    const auto& cell_bit = uniform_cell_bit_[bit_index];
    const auto& boundaries = uniform_boundaries_[bit_index];

    int32_t abs_coord = coord_q15 < 0 ? -static_cast<int32_t>(coord_q15) : coord_q15;
    int32_t cell_width = 2 * static_cast<int32_t>(norm_const_q15_);
    int c = (cell_width > 0) ? static_cast<int>(abs_coord / cell_width) : 0;
    if (c >= half_side_) {
        c = half_side_ - 1;
    }
    if (c < 0) {
        c = 0;
    }
    bool bit_here = cell_bit[static_cast<size_t>(c)] != 0;

    if (boundaries.empty()) {
        // This bit never changes across the whole axis (only possible
        // for a degenerate 1-cell axis, i.e. QPSK's half_side_==1, which
        // never reaches bit_index>=1 in the caller anyway). Defensive
        // fallback, not expected to be hit.
        return bit_here ? -abs_coord : abs_coord;
    }

    int32_t best_dist = std::numeric_limits<int32_t>::max();
    for (int32_t boundary : boundaries) {
        int32_t d = std::abs(abs_coord - boundary);
        if (d < best_dist) {
            best_dist = d;
        }
    }
    return bit_here ? -best_dist : best_dist;
}

void ConstellationDemapper::demap_uniform_fixed(sample_t symbol, int8_t* out) const {
    int16_t i_q15 = symbol.real();
    int16_t q_q15 = symbol.imag();
    size_t bits_per_axis = bits_per_symbol_ / 2;
    int64_t clip = config_.llr_clip;

    auto llr_from_dist = [&](int32_t dist_q15) -> int8_t {
        // dist_q15 is Q1.15 (true_dist * 2^15); llr_scale_q16_ is Q16.16
        // (true_scale * 2^16); combined factor 2^31.
        int64_t llr_raw = static_cast<int64_t>(dist_q15) * llr_scale_q16_;
        constexpr int kShift = 31;
        // Unconditional positive bias -- see compute_llr_max_log()'s
        // comment on this exact same fix (>> is floor-division, not
        // truncating, so the bias must not be sign-dependent).
        int64_t bias = int64_t(1) << (kShift - 1);
        int64_t llr = (llr_raw + bias) >> kShift;
        if (llr > clip)
            llr = clip;
        if (llr < -clip)
            llr = -clip;
        return static_cast<int8_t>(llr);
    };

    for (size_t b = 0; b < bits_per_axis; ++b) {
        int32_t dist = axis_bit_distance(i_q15, b);
        out[b] = llr_from_dist(dist);
    }
    for (size_t b = 0; b < bits_per_axis; ++b) {
        int32_t dist = axis_bit_distance(q_q15, b);
        out[bits_per_axis + b] = llr_from_dist(dist);
    }
}
#endif

void ConstellationDemapper::demap_symbol(sample_t symbol, int8_t* llr_out) const {
#ifdef ATSC3_FIXED_POINT
    // Phase 9.0b: the boundary slicer covers every uniform QAM order
    // (QPSK through 4096-QAM) directly on native Q1.15 samples -- no
    // q15_to_float, no denormalize round trip. NUC modes stay on
    // compute_llr_max_log() (now genuine fixed-point internally too).
    if (!is_nuc_modulation(config_.modulation)) {
        demap_uniform_fixed(symbol, llr_out);
        return;
    }
    for (size_t b = 0; b < bits_per_symbol_; ++b) {
        llr_out[b] = compute_llr_max_log(symbol, b);
    }
#else
    // Extract I/Q coordinates
    float i_coord = symbol.real();
    float q_coord = symbol.imag();

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
#endif
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

#ifdef ATSC3_FIXED_POINT
    // Phase 9.0b: one boundary slicer covers every uniform QAM order
    // directly (including 1024/4096-QAM, which previously had no fast
    // path and fell all the way through to the O(M) fallback below).
    if (!is_nuc_modulation(config_.modulation)) {
        for (size_t i = 0; i < num_symbols; ++i) {
            demap_uniform_fixed(symbols[i], out);
            out += bits_per_symbol_;
        }
        return num_symbols * bits_per_symbol_;
    }
#else
    // Use optimized batch processing for uniform QAM
    if (!is_nuc_modulation(config_.modulation)) {
        switch (config_.modulation) {
            case config::Modulation::QPSK:
                for (size_t i = 0; i < num_symbols; ++i) {
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
                    demap_qpsk_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 2;
                }
                return num_symbols * 2;

            case config::Modulation::QAM16:
                for (size_t i = 0; i < num_symbols; ++i) {
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
                    demap_qam16_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 4;
                }
                return num_symbols * 4;

            case config::Modulation::QAM64:
                for (size_t i = 0; i < num_symbols; ++i) {
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
                    demap_qam64_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 6;
                }
                return num_symbols * 6;

            case config::Modulation::QAM256:
                for (size_t i = 0; i < num_symbols; ++i) {
                    float i_c = symbols[i].real();
                    float q_c = symbols[i].imag();
                    demap_qam256_fast(i_c, q_c, llr_scale_, config_.llr_clip, out);
                    out += 8;
                }
                return num_symbols * 8;

            default:
                break;
        }
    }
#endif

    // Fallback for NUC (and, in the float build only, 1024/4096-QAM)
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
