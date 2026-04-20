// wiener_interpolator.cc — 2D Wiener channel estimate interpolation
//
// Implements optimal MMSE Wiener filtering for interpolating channel
// estimates from pilot positions to all data subcarriers across
// time and frequency dimensions.
//
// The filter coefficients are derived from channel autocorrelation
// functions (time-frequency correlation) for typical multipath profiles.
//
// Reference: Edfors et al., IEEE Trans. Comm. 1998

#include "wiener_interpolator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace channel {

namespace {

// Channel profile parameters for Wiener filter design
// Values derived from typical ATSC 3.0 reception scenarios
struct ChannelProfileParams {
    float max_doppler_hz;     // Maximum Doppler spread
    float rms_delay_spread_us; // RMS delay spread in microseconds
    float coherence_bw_hz;    // Approximate coherence bandwidth
};

ChannelProfileParams get_profile_params(ChannelProfile profile) {
    switch (profile) {
        case ChannelProfile::AWGN:
            return {0.0f, 0.0f, 1e9f};  // Infinite coherence
        case ChannelProfile::PEDESTRIAN:
            return {5.0f, 0.5f, 200000.0f};
        case ChannelProfile::VEHICULAR:
            return {50.0f, 2.0f, 50000.0f};
        case ChannelProfile::URBAN:
            return {80.0f, 5.0f, 20000.0f};
        case ChannelProfile::STATIC_MULTIPATH:
            return {0.0f, 3.0f, 33333.0f};
        default:
            return {10.0f, 1.0f, 100000.0f};
    }
}

// Compute Bessel J0 approximation for time-direction correlation
// Autocorrelation in time: R(Δt) = J0(2π * fd * Δt)
float bessel_j0(float x) {
    // Polynomial approximation for |x| <= 3
    float ax = std::abs(x);
    if (ax < 3.0f) {
        float y = x * x;
        return 1.0f - y * (0.25f - y * (0.015625f - y * 0.000434028f));
    } else {
        // Asymptotic approximation for large x
        float y = 3.0f / ax;
        float theta = ax - 0.785398163f;  // pi/4
        return std::sqrt(0.636619772f / ax) *
               (std::cos(theta) * (1.0f - 0.0625f * y * y) +
                std::sin(theta) * 0.125f * y);
    }
}

// Compute sinc function for frequency-direction correlation
// Autocorrelation in frequency: R(Δf) = sinc(Δf * τ_max)
float sinc(float x) {
    if (std::abs(x) < 1e-8f) {
        return 1.0f;
    }
    float pix = static_cast<float>(M_PI) * x;
    return std::sin(pix) / pix;
}

// Note: compute_correlation is reserved for future 2D Wiener filter design
// where both time and frequency correlation are needed together.
// Currently unused but kept for reference.

// Default OFDM parameters for ATSC 3.0 8K mode
constexpr float kDefaultSubcarrierSpacing = 843.75f;  // Hz (6.912 MHz / 8192)
constexpr float kDefaultSymbolDuration = 1185.0f;     // μs (including CP)

}  // namespace

// Compute Wiener filter coefficients
std::vector<float> compute_wiener_coefficients(
    float /*doppler_hz*/,  // Reserved for time-direction filtering
    float delay_spread_us,
    float snr_db,
    size_t num_taps) {

    std::vector<float> coeffs(num_taps);

    // Convert SNR to linear
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float noise_power = 1.0f / snr_linear;

    // For simplicity, compute 1D frequency-direction coefficients
    // using the autocorrelation function
    //
    // Wiener-Hopf: w = R_hh^{-1} * r_hd
    // For uniform pilot spacing, this simplifies to a Toeplitz matrix solve
    //
    // Here we use a simplified approach: approximate as weighted average
    // with weights proportional to correlation strength

    // Compute correlation values at pilot positions
    std::vector<float> corr(num_taps);
    float sum_corr = 0.0f;

    for (size_t i = 0; i < num_taps; ++i) {
        // Pilot separation in subcarriers (assuming pilot spacing of 6)
        int delta = static_cast<int>(i) - static_cast<int>(num_taps / 2);

        // Frequency correlation
        corr[i] = sinc(static_cast<float>(delta) * 6.0f *
                       kDefaultSubcarrierSpacing * delay_spread_us * 1e-6f);
        corr[i] = std::max(0.0f, corr[i]);  // Ensure non-negative
        sum_corr += corr[i];
    }

    // Normalize and apply MMSE regularization
    if (sum_corr > 0.0f) {
        for (size_t i = 0; i < num_taps; ++i) {
            // MMSE: scale by signal / (signal + noise)
            coeffs[i] = corr[i] / (sum_corr + noise_power);
        }
    } else {
        // Fallback: uniform weights
        float uniform = 1.0f / static_cast<float>(num_taps);
        std::fill(coeffs.begin(), coeffs.end(), uniform);
    }

    // Normalize to sum to 1
    float coeff_sum = 0.0f;
    for (float c : coeffs) coeff_sum += c;
    if (coeff_sum > 0.0f) {
        for (float& c : coeffs) c /= coeff_sum;
    }

    return coeffs;
}

WienerInterpolator::WienerInterpolator(const WienerInterpolatorConfig& config)
    : config_(config) {
    init();
}

WienerInterpolator::~WienerInterpolator() = default;

void WienerInterpolator::init() {
    compute_filter_coefficients();

    // Pre-allocate history buffer
    history_.resize(config_.time_taps);
    for (auto& h : history_) {
        h.resize(config_.num_active_carriers, sample_t(0, 0));
    }
    history_write_idx_ = 0;
    history_filled_ = false;
}

void WienerInterpolator::compute_filter_coefficients() {
    auto params = get_profile_params(config_.profile);

    // Frequency-direction coefficients
    float snr_db = -10.0f * std::log10(config_.noise_variance + 1e-10f);
    freq_coeffs_ = compute_wiener_coefficients(
        params.max_doppler_hz,
        params.rms_delay_spread_us,
        snr_db,
        config_.freq_taps);

    // Time-direction coefficients (using Doppler correlation)
    time_coeffs_.resize(config_.time_taps);
    float time_sum = 0.0f;

    for (size_t i = 0; i < config_.time_taps; ++i) {
        int delta = static_cast<int>(i) - static_cast<int>(config_.time_taps / 2);

        // Time correlation: J0(2π * fd * Δl * Tsym)
        time_coeffs_[i] = bessel_j0(2.0f * static_cast<float>(M_PI) *
                                     params.max_doppler_hz *
                                     static_cast<float>(delta) *
                                     static_cast<float>(config_.pilot_spacing_time) *
                                     kDefaultSymbolDuration * 1e-6f);
        time_coeffs_[i] = std::max(0.0f, time_coeffs_[i]);
        time_sum += time_coeffs_[i];
    }

    // Normalize time coefficients
    if (time_sum > 0.0f) {
        for (float& c : time_coeffs_) c /= time_sum;
    }
}

void WienerInterpolator::reset() {
    for (auto& h : history_) {
        std::fill(h.begin(), h.end(), sample_t(0, 0));
    }
    history_write_idx_ = 0;
    history_filled_ = false;
}

void WienerInterpolator::set_config(const WienerInterpolatorConfig& config) {
    config_ = config;
    init();
}

std::vector<sample_t> WienerInterpolator::interpolate(
    const std::vector<sample_t>& pilot_estimates,
    const std::vector<size_t>& pilot_positions) const {

    std::vector<sample_t> output(config_.num_active_carriers);
    interpolate_frequency(pilot_estimates, pilot_positions, output);
    return output;
}

std::vector<sample_t> WienerInterpolator::interpolate_with_history(
    const std::vector<sample_t>& pilot_estimates,
    const std::vector<size_t>& pilot_positions,
    size_t /*symbol_index*/) {

    // First do frequency interpolation
    std::vector<sample_t> freq_interp(config_.num_active_carriers);
    interpolate_frequency(pilot_estimates, pilot_positions, freq_interp);

    // Store in history
    history_[history_write_idx_] = freq_interp;
    history_write_idx_ = (history_write_idx_ + 1) % config_.time_taps;

    if (history_write_idx_ == 0) {
        history_filled_ = true;
    }

    // If we have enough history, apply time filtering
    if (history_filled_) {
        filter_time(freq_interp);
    }

    return freq_interp;
}

void WienerInterpolator::interpolate_frequency(
    const std::vector<sample_t>& pilot_estimates,
    const std::vector<size_t>& pilot_positions,
    std::vector<sample_t>& output) const {

    if (pilot_estimates.empty() || pilot_positions.empty()) {
        std::fill(output.begin(), output.end(), sample_t(0, 0));
        return;
    }

    size_t num_pilots = pilot_estimates.size();
    size_t num_taps = freq_coeffs_.size();
    size_t half_taps = num_taps / 2;

    // Sort pilots by position
    std::vector<std::pair<size_t, size_t>> sorted_indices(num_pilots);
    for (size_t i = 0; i < num_pilots; ++i) {
        sorted_indices[i] = {pilot_positions[i], i};
    }
    std::sort(sorted_indices.begin(), sorted_indices.end());

    // For each output position, apply Wiener filter using nearby pilots
    for (size_t k = 0; k < config_.num_active_carriers; ++k) {
        // Find closest pilot index
        size_t closest_idx = 0;
        size_t min_dist = config_.num_active_carriers;
        for (size_t i = 0; i < num_pilots; ++i) {
            size_t pos = sorted_indices[i].first;
            size_t dist = (k >= pos) ? (k - pos) : (pos - k);
            if (dist < min_dist) {
                min_dist = dist;
                closest_idx = i;
            }
        }

        // Find range of pilots to use for filtering
        size_t start_idx = (closest_idx >= half_taps) ? (closest_idx - half_taps) : 0;
        size_t end_idx = std::min(num_pilots, closest_idx + half_taps + 1);
        size_t actual_taps = end_idx - start_idx;

        // Compute weighted average
#ifdef ATSC3_FIXED_POINT
        int32_t sum_re = 0;
        int32_t sum_im = 0;
#else
        float sum_re = 0.0f;
        float sum_im = 0.0f;
#endif
        float weight_sum = 0.0f;

        for (size_t i = start_idx; i < end_idx; ++i) {
            size_t orig_idx = sorted_indices[i].second;
            size_t pilot_pos = sorted_indices[i].first;
            const sample_t& h = pilot_estimates[orig_idx];

            // Distance-based weight modification
            size_t dist = (k >= pilot_pos) ? (k - pilot_pos) : (pilot_pos - k);
            float base_weight = (i - start_idx < freq_coeffs_.size())
                                    ? freq_coeffs_[i - start_idx]
                                    : (1.0f / static_cast<float>(actual_taps));

            // Apply distance decay
            float weight = base_weight / (1.0f + static_cast<float>(dist) / 10.0f);
            weight_sum += weight;

#ifdef ATSC3_FIXED_POINT
            sum_re += static_cast<int32_t>(weight * static_cast<float>(h.real()));
            sum_im += static_cast<int32_t>(weight * static_cast<float>(h.imag()));
#else
            sum_re += weight * h.real();
            sum_im += weight * h.imag();
#endif
        }

        // Normalize
        if (weight_sum > 0.0f) {
#ifdef ATSC3_FIXED_POINT
            int32_t out_re = static_cast<int32_t>(static_cast<float>(sum_re) / weight_sum);
            int32_t out_im = static_cast<int32_t>(static_cast<float>(sum_im) / weight_sum);
            out_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, out_re));
            out_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, out_im));
            output[k] = sample_t(static_cast<int16_t>(out_re),
                                  static_cast<int16_t>(out_im));
#else
            output[k] = sample_t(sum_re / weight_sum, sum_im / weight_sum);
#endif
        } else {
            output[k] = sample_t(0, 0);
        }
    }
}

void WienerInterpolator::filter_time(std::vector<sample_t>& estimate) {
    if (!history_filled_) {
        return;
    }

    size_t num_carriers = estimate.size();
    size_t num_taps = time_coeffs_.size();

    for (size_t k = 0; k < num_carriers; ++k) {
#ifdef ATSC3_FIXED_POINT
        int32_t sum_re = 0;
        int32_t sum_im = 0;
#else
        float sum_re = 0.0f;
        float sum_im = 0.0f;
#endif

        for (size_t t = 0; t < num_taps; ++t) {
            // Access history in correct order (oldest to newest)
            size_t hist_idx = (history_write_idx_ + t) % num_taps;
            const sample_t& h = history_[hist_idx][k];
            float weight = time_coeffs_[t];

#ifdef ATSC3_FIXED_POINT
            sum_re += static_cast<int32_t>(weight * static_cast<float>(h.real()));
            sum_im += static_cast<int32_t>(weight * static_cast<float>(h.imag()));
#else
            sum_re += weight * h.real();
            sum_im += weight * h.imag();
#endif
        }

#ifdef ATSC3_FIXED_POINT
        sum_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, sum_re));
        sum_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, sum_im));
        estimate[k] = sample_t(static_cast<int16_t>(sum_re),
                                static_cast<int16_t>(sum_im));
#else
        estimate[k] = sample_t(sum_re, sum_im);
#endif
    }
}

sample_t WienerInterpolator::apply_fir(
    const std::vector<sample_t>& input,
    size_t center_idx,
    const std::vector<float>& coeffs) const {

    size_t num_taps = coeffs.size();
    size_t half_taps = num_taps / 2;
    size_t input_size = input.size();

#ifdef ATSC3_FIXED_POINT
    int32_t sum_re = 0;
    int32_t sum_im = 0;
#else
    float sum_re = 0.0f;
    float sum_im = 0.0f;
#endif

    for (size_t i = 0; i < num_taps; ++i) {
        int64_t idx = static_cast<int64_t>(center_idx) -
                      static_cast<int64_t>(half_taps) +
                      static_cast<int64_t>(i);

        // Handle boundary conditions (zero-padding)
        if (idx < 0 || idx >= static_cast<int64_t>(input_size)) {
            continue;
        }

        const sample_t& s = input[static_cast<size_t>(idx)];
        float w = coeffs[i];

#ifdef ATSC3_FIXED_POINT
        sum_re += static_cast<int32_t>(w * static_cast<float>(s.real()));
        sum_im += static_cast<int32_t>(w * static_cast<float>(s.imag()));
#else
        sum_re += w * s.real();
        sum_im += w * s.imag();
#endif
    }

#ifdef ATSC3_FIXED_POINT
    sum_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, sum_re));
    sum_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, sum_im));
    return sample_t(static_cast<int16_t>(sum_re), static_cast<int16_t>(sum_im));
#else
    return sample_t(sum_re, sum_im);
#endif
}

}  // namespace channel
}  // namespace atsc3
