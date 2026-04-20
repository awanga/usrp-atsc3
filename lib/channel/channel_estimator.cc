// channel_estimator.cc — ATSC 3.0 OFDM channel estimation
//
// Computes channel frequency response H(f) from pilot observations
// using Least-Squares (LS) estimation at pilot positions.
//
// Reference: ATSC A/322 Section 7, Edfors et al. IEEE Trans. Comm. 1998

#include "channel_estimator.h"

#include "../ofdm/pilot_extractor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace channel {

namespace {

// Active carrier counts per FFT size (from ATSC A/322)
constexpr size_t kActiveCarriers8K = 6913;
constexpr size_t kActiveCarriers16K = 13825;
constexpr size_t kActiveCarriers32K = 27649;

// Compute first active subcarrier (center FFT around DC)
size_t compute_first_active(size_t fft_size, size_t num_active) {
    return (fft_size - num_active) / 2;
}

// Get active carrier count for FFT size
size_t get_active_carriers(size_t fft_size) {
    switch (fft_size) {
        case 8192:
            return kActiveCarriers8K;
        case 16384:
            return kActiveCarriers16K;
        case 32768:
            return kActiveCarriers32K;
        default:
            return 0;
    }
}

// Minimum magnitude for division to avoid numerical issues
#ifdef ATSC3_FIXED_POINT
constexpr int32_t kMinMagnitudeSq = 100;  // ~0.003 in Q15
#else
constexpr float kMinMagnitudeSq = 1e-6f;
#endif

}  // namespace

// Safe complex division with saturation
sample_t safe_complex_divide(sample_t a, sample_t b) {
#ifdef ATSC3_FIXED_POINT
    // Fixed-point division: a / b = a * conj(b) / |b|^2
    // Use int32_t intermediates to avoid overflow

    int32_t b_re = b.real();
    int32_t b_im = b.imag();

    // |b|^2 in Q30 (Q15 * Q15 = Q30)
    int32_t mag_sq = b_re * b_re + b_im * b_im;

    // Avoid division by very small numbers
    if (mag_sq < kMinMagnitudeSq) {
        return sample_t(0, 0);
    }

    int32_t a_re = a.real();
    int32_t a_im = a.imag();

    // a * conj(b) in Q30
    int32_t num_re = a_re * b_re + a_im * b_im;
    int32_t num_im = a_im * b_re - a_re * b_im;

    // Result in Q15: (Q30 / Q30) << 15 = Q15
    // We need to scale properly: result = (num / mag_sq)
    // Since num is Q30 and mag_sq is Q30, result would be Q0
    // We want Q15, so we shift num left by 15 before division
    // To avoid overflow, we shift num right by 15 first, then divide

    int32_t result_re = (num_re << 0) / (mag_sq >> 15);
    int32_t result_im = (num_im << 0) / (mag_sq >> 15);

    // Clamp to int16_t range
    result_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, result_re));
    result_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, result_im));

    return sample_t(static_cast<int16_t>(result_re), static_cast<int16_t>(result_im));
#else
    // Float division
    float mag_sq = b.real() * b.real() + b.imag() * b.imag();

    if (mag_sq < kMinMagnitudeSq) {
        return sample_t(0.0f, 0.0f);
    }

    // a / b = a * conj(b) / |b|^2
    float result_re = (a.real() * b.real() + a.imag() * b.imag()) / mag_sq;
    float result_im = (a.imag() * b.real() - a.real() * b.imag()) / mag_sq;

    return sample_t(result_re, result_im);
#endif
}

ChannelEstimator::ChannelEstimator(const ChannelEstimatorConfig& config) : config_(config) {
    init();
}

ChannelEstimator::~ChannelEstimator() = default;

void ChannelEstimator::init() {
    // Auto-compute active carrier parameters if not specified
    if (config_.num_active_carriers == 0) {
        config_.num_active_carriers = get_active_carriers(config_.fft_size);
        if (config_.num_active_carriers == 0) {
            throw std::invalid_argument("Invalid FFT size for channel estimation");
        }
    }

    if (config_.first_active_carrier == 0) {
        config_.first_active_carrier =
            compute_first_active(config_.fft_size, config_.num_active_carriers);
    }

    // Pre-allocate averaging buffer
    avg_estimate_.resize(config_.num_active_carriers, sample_t(0, 0));
    has_prev_estimate_ = false;
}

void ChannelEstimator::reset() {
    has_prev_estimate_ = false;
    std::fill(avg_estimate_.begin(), avg_estimate_.end(), sample_t(0, 0));
    last_snr_db_ = 0.0f;
}

void ChannelEstimator::set_config(const ChannelEstimatorConfig& config) {
    config_ = config;
    init();
    reset();
}

void ChannelEstimator::set_backend(EstimatorBackend backend) {
    if (backend == EstimatorBackend::ML_ONNX) {
        throw std::runtime_error("ML_ONNX backend not implemented (post-MVP)");
    }
    config_.backend = backend;
}

size_t ChannelEstimator::get_num_active_carriers() const {
    return config_.num_active_carriers;
}

size_t ChannelEstimator::get_first_active_carrier() const {
    return config_.first_active_carrier;
}

ChannelEstimate ChannelEstimator::estimate(const std::vector<ofdm::PilotSymbol>& pilots,
                                           size_t symbol_index) {
    ChannelEstimate result;
    result.symbol_index = symbol_index;

    if (pilots.empty()) {
        result.is_valid = false;
        return result;
    }

    // Step 1: Compute LS estimate at pilot positions
    std::vector<sample_t> pilot_h = compute_ls_estimate(pilots);

    // Extract pilot positions (relative to first active carrier)
    std::vector<size_t> pilot_positions;
    pilot_positions.reserve(pilots.size());
    for (const auto& pilot : pilots) {
        if (pilot.subcarrier_index >= config_.first_active_carrier) {
            pilot_positions.push_back(pilot.subcarrier_index - config_.first_active_carrier);
        }
    }

    // Step 2: Interpolate to all active subcarriers
    result.h_estimate.resize(config_.num_active_carriers);
    interpolate(pilot_h, pilot_positions, result.h_estimate);

    // Step 3: Apply temporal averaging if enabled
    if (config_.enable_averaging) {
        apply_averaging(result.h_estimate);
    }

    // Step 4: Compute quality metrics
    result.estimated_snr_db = estimate_snr(pilots, pilot_h);
    result.mean_phase_error = compute_mean_phase_error(pilots);
    last_snr_db_ = result.estimated_snr_db;

    result.is_valid = true;
    return result;
}

std::vector<sample_t>
ChannelEstimator::estimate_at_pilots(const std::vector<ofdm::PilotSymbol>& pilots) const {
    return compute_ls_estimate(pilots);
}

std::vector<sample_t>
ChannelEstimator::compute_ls_estimate(const std::vector<ofdm::PilotSymbol>& pilots) const {
    std::vector<sample_t> h_estimates;
    h_estimates.reserve(pilots.size());

    for (const auto& pilot : pilots) {
        // LS estimate: H = Y / X (received / reference)
        // For BPSK pilots, reference is real-valued, so this simplifies
        sample_t h = safe_complex_divide(pilot.received_value, pilot.reference_value);
        h_estimates.push_back(h);
    }

    return h_estimates;
}

void ChannelEstimator::interpolate(const std::vector<sample_t>& pilot_estimates,
                                   const std::vector<size_t>& pilot_positions,
                                   std::vector<sample_t>& full_estimate) const {
    if (pilot_estimates.empty() || pilot_positions.empty()) {
        std::fill(full_estimate.begin(), full_estimate.end(), sample_t(0, 0));
        return;
    }

    switch (config_.interpolation) {
        case InterpolationMethod::LINEAR:
        case InterpolationMethod::CUBIC:   // Fall back to linear for now
        case InterpolationMethod::WIENER:  // Fall back to linear for now
            interpolate_linear(pilot_estimates, pilot_positions, full_estimate);
            break;
    }
}

void ChannelEstimator::interpolate_linear(const std::vector<sample_t>& pilot_estimates,
                                          const std::vector<size_t>& pilot_positions,
                                          std::vector<sample_t>& full_estimate) const {
    size_t num_pilots = pilot_estimates.size();
    size_t num_carriers = full_estimate.size();

    if (num_pilots == 0) {
        return;
    }

    // Handle single pilot case (constant extrapolation)
    if (num_pilots == 1) {
        std::fill(full_estimate.begin(), full_estimate.end(), pilot_estimates[0]);
        return;
    }

    // Sort pilots by position (should already be sorted, but verify)
    std::vector<std::pair<size_t, size_t>> sorted_indices(num_pilots);
    for (size_t i = 0; i < num_pilots; ++i) {
        sorted_indices[i] = {pilot_positions[i], i};
    }
    std::sort(sorted_indices.begin(), sorted_indices.end());

    // Linear interpolation between pilots
    size_t pilot_idx = 0;

    for (size_t k = 0; k < num_carriers; ++k) {
        // Find surrounding pilots
        while (pilot_idx < num_pilots - 1 && sorted_indices[pilot_idx + 1].first <= k) {
            ++pilot_idx;
        }

        size_t pos_left = sorted_indices[pilot_idx].first;
        size_t idx_left = sorted_indices[pilot_idx].second;
        sample_t h_left = pilot_estimates[idx_left];

        // Before first pilot: extrapolate
        if (k < pos_left) {
            if (pilot_idx == 0 && num_pilots > 1) {
                // Extrapolate using first two pilots
                size_t pos_right = sorted_indices[1].first;
                size_t idx_right = sorted_indices[1].second;
                sample_t h_right = pilot_estimates[idx_right];

                if (pos_right > pos_left) {
#ifdef ATSC3_FIXED_POINT
                    int32_t denom = static_cast<int32_t>(pos_right - pos_left);
                    int32_t numer = static_cast<int32_t>(k - pos_left);
                    int32_t h_re =
                        h_left.real() + ((h_right.real() - h_left.real()) * numer) / denom;
                    int32_t h_im =
                        h_left.imag() + ((h_right.imag() - h_left.imag()) * numer) / denom;
                    h_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_re));
                    h_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_im));
                    full_estimate[k] =
                        sample_t(static_cast<int16_t>(h_re), static_cast<int16_t>(h_im));
#else
                    float alpha =
                        static_cast<float>(k - pos_left) / static_cast<float>(pos_right - pos_left);
                    full_estimate[k] =
                        sample_t(h_left.real() + alpha * (h_right.real() - h_left.real()),
                                 h_left.imag() + alpha * (h_right.imag() - h_left.imag()));
#endif
                } else {
                    full_estimate[k] = h_left;
                }
            } else {
                full_estimate[k] = h_left;
            }
            continue;
        }

        // After last pilot: extrapolate
        if (pilot_idx >= num_pilots - 1) {
            full_estimate[k] = h_left;
            continue;
        }

        // Between pilots: interpolate
        size_t pos_right = sorted_indices[pilot_idx + 1].first;
        size_t idx_right = sorted_indices[pilot_idx + 1].second;
        sample_t h_right = pilot_estimates[idx_right];

        if (pos_right == pos_left) {
            full_estimate[k] = h_left;
            continue;
        }

#ifdef ATSC3_FIXED_POINT
        // Fixed-point linear interpolation
        // h = h_left + (h_right - h_left) * (k - pos_left) / (pos_right - pos_left)
        int32_t denom = static_cast<int32_t>(pos_right - pos_left);
        int32_t numer = static_cast<int32_t>(k - pos_left);

        int32_t delta_re = h_right.real() - h_left.real();
        int32_t delta_im = h_right.imag() - h_left.imag();

        int32_t h_re = h_left.real() + (delta_re * numer) / denom;
        int32_t h_im = h_left.imag() + (delta_im * numer) / denom;

        // Clamp to int16_t range
        h_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_re));
        h_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, h_im));

        full_estimate[k] = sample_t(static_cast<int16_t>(h_re), static_cast<int16_t>(h_im));
#else
        // Float linear interpolation
        float alpha = static_cast<float>(k - pos_left) / static_cast<float>(pos_right - pos_left);

        full_estimate[k] = sample_t(h_left.real() + alpha * (h_right.real() - h_left.real()),
                                    h_left.imag() + alpha * (h_right.imag() - h_left.imag()));
#endif
    }
}

void ChannelEstimator::apply_averaging(std::vector<sample_t>& estimate) {
    if (!has_prev_estimate_) {
        // First estimate, just store it
        avg_estimate_ = estimate;
        has_prev_estimate_ = true;
        return;
    }

    // IIR averaging: avg = alpha * new + (1-alpha) * old
    float alpha = config_.averaging_alpha;

    for (size_t k = 0; k < estimate.size() && k < avg_estimate_.size(); ++k) {
#ifdef ATSC3_FIXED_POINT
        // Fixed-point IIR filter
        // Scale alpha to Q15 format (alpha * 32768)
        int32_t alpha_q15 = static_cast<int32_t>(alpha * 32768.0f);
        int32_t one_minus_alpha_q15 = 32768 - alpha_q15;

        int32_t new_re =
            (alpha_q15 * estimate[k].real() + one_minus_alpha_q15 * avg_estimate_[k].real()) >> 15;
        int32_t new_im =
            (alpha_q15 * estimate[k].imag() + one_minus_alpha_q15 * avg_estimate_[k].imag()) >> 15;

        // Clamp
        new_re = std::max<int32_t>(-32767, std::min<int32_t>(32767, new_re));
        new_im = std::max<int32_t>(-32767, std::min<int32_t>(32767, new_im));

        avg_estimate_[k] = sample_t(static_cast<int16_t>(new_re), static_cast<int16_t>(new_im));
#else
        float one_minus_alpha = 1.0f - alpha;
        avg_estimate_[k] =
            sample_t(alpha * estimate[k].real() + one_minus_alpha * avg_estimate_[k].real(),
                     alpha * estimate[k].imag() + one_minus_alpha * avg_estimate_[k].imag());
#endif
    }

    estimate = avg_estimate_;
}

float ChannelEstimator::estimate_snr(const std::vector<ofdm::PilotSymbol>& pilots,
                                     const std::vector<sample_t>& h_estimates) const {
    if (pilots.size() < 2 || h_estimates.size() < 2) {
        return 0.0f;
    }

    // Estimate SNR from channel estimate variance between adjacent pilots.
    // For a slowly-varying channel, adjacent pilots should have similar H.
    // The difference between adjacent H estimates is dominated by noise.
    //
    // Method: Compare variance of H differences to mean |H|^2
    // SNR ≈ E[|H|^2] / (E[|H[k+1] - H[k]|^2] / 2)
    //
    // The factor of 2 accounts for noise appearing in both H[k] and H[k+1]

    float signal_power = 0.0f;
    float diff_power = 0.0f;
    size_t count = std::min(pilots.size(), h_estimates.size());

    for (size_t i = 0; i < count; ++i) {
        const auto& h = h_estimates[i];

#ifdef ATSC3_FIXED_POINT
        float h_re = q15_to_float(h.real());
        float h_im = q15_to_float(h.imag());
#else
        float h_re = h.real();
        float h_im = h.imag();
#endif

        signal_power += h_re * h_re + h_im * h_im;

        // Compute difference to next estimate for noise estimation
        if (i + 1 < count) {
            const auto& h_next = h_estimates[i + 1];

#ifdef ATSC3_FIXED_POINT
            float h_next_re = q15_to_float(h_next.real());
            float h_next_im = q15_to_float(h_next.imag());
#else
            float h_next_re = h_next.real();
            float h_next_im = h_next.imag();
#endif

            float diff_re = h_next_re - h_re;
            float diff_im = h_next_im - h_im;
            diff_power += diff_re * diff_re + diff_im * diff_im;
        }
    }

    // Average signal power
    signal_power /= static_cast<float>(count);

    // Average difference power (n-1 differences)
    // Noise power = diff_power / 2 (each difference has noise from both)
    float noise_power = diff_power / static_cast<float>(2 * (count - 1));

    // Compute SNR in dB
    if (noise_power < 1e-10f) {
        return 100.0f;  // Effectively infinite SNR
    }

    float snr_linear = signal_power / noise_power;
    return 10.0f * std::log10(snr_linear);
}

float ChannelEstimator::compute_mean_phase_error(
    const std::vector<ofdm::PilotSymbol>& pilots) const {
    if (pilots.empty()) {
        return 0.0f;
    }

    float total_phase = 0.0f;
    size_t count = 0;

    for (const auto& pilot : pilots) {
#ifdef ATSC3_FIXED_POINT
        float ref_re = q15_to_float(pilot.reference_value.real());
        float rec_re = q15_to_float(pilot.received_value.real());
        float rec_im = q15_to_float(pilot.received_value.imag());
#else
        float ref_re = pilot.reference_value.real();
        float rec_re = pilot.received_value.real();
        float rec_im = pilot.received_value.imag();
#endif

        // Phase error is angle of (received * conj(reference))
        // For BPSK reference: atan2(imag * ref, real * ref)
        float phase = std::atan2(rec_im * ref_re, rec_re * ref_re);
        total_phase += phase;
        ++count;
    }

    return (count > 0) ? (total_phase / static_cast<float>(count)) : 0.0f;
}

}  // namespace channel
}  // namespace atsc3
