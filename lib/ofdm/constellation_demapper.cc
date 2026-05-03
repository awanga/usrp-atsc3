// constellation_demapper.cc — ATSC 3.0 QAM symbol to soft LLR conversion
//
// Computes soft LLRs from equalized QAM symbols for LDPC decoding.
//
// Reference: ATSC A/322 Section 7.5 (Non-Uniform Constellations)

#include "constellation_demapper.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace atsc3 {
namespace ofdm {

namespace {

// Gray code for bit position extraction
// Returns the Gray-coded index for a given linear index
inline size_t to_gray(size_t n) {
    return n ^ (n >> 1);
}

// Inverse Gray code
inline size_t from_gray(size_t g) {
    size_t n = g;
    for (size_t mask = n >> 1; mask != 0; mask >>= 1) {
        n ^= mask;
    }
    return n;
}

// Uniform QAM normalization factors (to unit average power)
// sqrt(2/(M-1)) for M-QAM where symbols are at ±1, ±3, ...
constexpr float kQamNorm4 = 1.0f / std::sqrt(2.0f);        // QPSK
constexpr float kQamNorm16 = 1.0f / std::sqrt(10.0f);      // 16-QAM
constexpr float kQamNorm64 = 1.0f / std::sqrt(42.0f);      // 64-QAM
constexpr float kQamNorm256 = 1.0f / std::sqrt(170.0f);    // 256-QAM
constexpr float kQamNorm1024 = 1.0f / std::sqrt(682.0f);   // 1024-QAM
constexpr float kQamNorm4096 = 1.0f / std::sqrt(2730.0f);  // 4096-QAM

// NUC constellation tables per ATSC A/322 Table 7.5
// Format: {real, imag} pairs normalized to unit average power
// Tables indexed by code rate: 2/15, 3/15, ..., 13/15

// NUC-16 constellation points (16-QAM NUC)
// Simplified: using uniform 16-QAM scaled for now
// Full NUC tables would be code-rate dependent per ATSC A/322
const std::vector<std::pair<float, float>> kNuc16Default = {
    // First quadrant (I+, Q+), then rotated for other quadrants
    {0.2236f, 0.2236f},
    {0.2236f, 0.6708f},
    {0.6708f, 0.2236f},
    {0.6708f, 0.6708f},
    // I-, Q+
    {-0.2236f, 0.2236f},
    {-0.2236f, 0.6708f},
    {-0.6708f, 0.2236f},
    {-0.6708f, 0.6708f},
    // I+, Q-
    {0.2236f, -0.2236f},
    {0.2236f, -0.6708f},
    {0.6708f, -0.2236f},
    {0.6708f, -0.6708f},
    // I-, Q-
    {-0.2236f, -0.2236f},
    {-0.2236f, -0.6708f},
    {-0.6708f, -0.2236f},
    {-0.6708f, -0.6708f}};

// NUC-64 constellation points (placeholder - uses uniform 64-QAM pattern)
// Full ATSC 3.0 NUC tables are code-rate dependent

// Helper to build Gray-coded QAM constellation
std::vector<sample_t> build_uniform_qam(size_t bits_per_symbol, float norm) {
    size_t m = 1u << bits_per_symbol;
    size_t side = 1u << (bits_per_symbol / 2);

    std::vector<sample_t> points(m);

    for (size_t i = 0; i < m; ++i) {
        // Extract I and Q indices using Gray coding
        size_t i_idx = from_gray(i >> (bits_per_symbol / 2));
        size_t q_idx = from_gray(i & ((1u << (bits_per_symbol / 2)) - 1));

        // Map to symmetric constellation: -(M-1), -(M-3), ..., -1, +1, ..., +(M-1)
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

// Build QPSK constellation (special case: not square)
std::vector<sample_t> build_qpsk() {
    std::vector<sample_t> points(4);

    // QPSK at ±1/√2, ±1/√2 (unit power)
    constexpr float val = 0.7071067811865476f;  // 1/√2

#ifdef ATSC3_FIXED_POINT
    // Q1: 00
    points[0] = sample_t(float_to_q15(val), float_to_q15(val));
    // Q2: 01
    points[1] = sample_t(float_to_q15(-val), float_to_q15(val));
    // Q3: 11
    points[2] = sample_t(float_to_q15(-val), float_to_q15(-val));
    // Q4: 10
    points[3] = sample_t(float_to_q15(val), float_to_q15(-val));
#else
    points[0] = sample_t(val, val);
    points[1] = sample_t(-val, val);
    points[2] = sample_t(-val, -val);
    points[3] = sample_t(val, -val);
#endif

    return points;
}

}  // namespace

ConstellationDemapper::ConstellationDemapper(const DemapperConfig& config) : config_(config) {
    init();
}

ConstellationDemapper::~ConstellationDemapper() = default;

void ConstellationDemapper::init() {
    // Determine bits per symbol
    bits_per_symbol_ = get_bits_per_symbol(config_.modulation);

    // Generate constellation points
    if (is_nuc_modulation(config_.modulation)) {
        generate_nuc_constellation();
    } else {
        generate_uniform_constellation();
    }

    // Build bit mapping tables
    build_bit_mappings();

    // Compute LLR scaling factor
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
    // NUC constellations are code-rate dependent per ATSC A/322 Table 7.5
    // For now, use simplified tables (uniform QAM as placeholder)
    // Full implementation would load rate-specific tables from JSON

    size_t m = get_constellation_size(config_.modulation);
    constellation_points_.resize(m);

    switch (config_.modulation) {
        case config::Modulation::NUC_QPSK:
            // NUC-QPSK is same as uniform QPSK
            constellation_points_ = build_qpsk();
            break;

        case config::Modulation::NUC_16:
            // Use NUC-16 table
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
            // Use uniform 64-QAM as placeholder
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
    // Max-log LLR: min_{s: b=0} |y-s|² - min_{s: b=1} |y-s|²
    // Positive LLR means bit=0 is more likely

    float min_dist_0 = std::numeric_limits<float>::max();
    float min_dist_1 = std::numeric_limits<float>::max();

    // Find minimum distance to points where bit=0
    for (size_t idx : bit_clears_[bit_index]) {
        float dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_0) {
            min_dist_0 = dist;
        }
    }

    // Find minimum distance to points where bit=1
    for (size_t idx : bit_sets_[bit_index]) {
        float dist = squared_distance(symbol, constellation_points_[idx]);
        if (dist < min_dist_1) {
            min_dist_1 = dist;
        }
    }

    // LLR = (min_dist_1 - min_dist_0) / noise_variance
    // Note: sign convention - positive means bit=0 more likely
    float llr = (min_dist_1 - min_dist_0) * llr_scale_;

    // Clamp to int8_t range
    float clip = static_cast<float>(config_.llr_clip);
    llr = std::max(-clip, std::min(clip, llr));

    return static_cast<int8_t>(std::round(llr));
}

void ConstellationDemapper::demap_symbol(sample_t symbol, int8_t* llr_out) const {
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
    // No state to reset currently
}

}  // namespace ofdm
}  // namespace atsc3
