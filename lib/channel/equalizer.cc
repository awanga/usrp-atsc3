// equalizer.cc — ATSC 3.0 frequency-domain equalizer
//
// Implements single-tap FDE with ZF and MMSE modes.
// Includes residual phase noise tracking from continual pilots.
//
// Reference: ATSC A/322 Section 7

#include "equalizer.h"

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

// Minimum magnitude squared for equalization
#ifdef ATSC3_FIXED_POINT
// Must be >= 2^15 (1<<15 = 32768): both equalize_zf's `denom = mag_sq
// >> 15` and equalize_mmse's `denom = denom_q30 >> 15` are zero below
// this, and the fallback-to-1 divisor that guards the divide-by-zero
// masks a real deep fade as a near-full-scale output instead of the
// zeroed carrier this check is meant to produce. This floor is
// unconditional -- config_.min_channel_magnitude is a separate,
// user-tunable deep-fade threshold on top of it, not a replacement for
// it (equalize_mmse doesn't consult min_channel_magnitude at all, so
// this constant is its only protection).
constexpr int32_t kMinMagnitudeSqDefault = 1 << 15;
#else
constexpr float kMinMagnitudeSqDefault = 1e-6f;
#endif

}  // namespace

Equalizer::Equalizer(const EqualizerConfig& config) : config_(config) {
    init();
}

Equalizer::~Equalizer() = default;

void Equalizer::init() {
    // Auto-compute active carrier parameters if not specified
    if (config_.num_active_carriers == 0) {
        config_.num_active_carriers = get_active_carriers(config_.fft_size);
        if (config_.num_active_carriers == 0) {
            throw std::invalid_argument("Invalid FFT size for equalizer");
        }
    }

    if (config_.first_active_carrier == 0) {
        config_.first_active_carrier =
            compute_first_active(config_.fft_size, config_.num_active_carriers);
    }

    reset();
}

void Equalizer::reset() {
    residual_phase_ = 0.0f;
#ifdef ATSC3_FIXED_POINT
    phase_phasor_ = sample_t(32767, 0);  // 1 + 0j in Q15
#else
    phase_phasor_ = sample_t(1.0f, 0.0f);
#endif
}

void Equalizer::set_config(const EqualizerConfig& config) {
    config_ = config;
    init();
}

size_t Equalizer::get_num_active_carriers() const {
    return config_.num_active_carriers;
}

size_t Equalizer::get_first_active_carrier() const {
    return config_.first_active_carrier;
}

sample_t Equalizer::equalize_zf(sample_t y, sample_t h) const {
    // ZF: X_hat = Y / H
    // Implemented as: X_hat = Y * conj(H) / |H|^2

#ifdef ATSC3_FIXED_POINT
    int32_t h_re = h.real();
    int32_t h_im = h.imag();

    // |H|^2 in Q30
    int32_t mag_sq = h_re * h_re + h_im * h_im;

    // Check for very small channel (deep fade)
    int32_t min_mag_sq =
        static_cast<int32_t>(config_.min_channel_magnitude * config_.min_channel_magnitude *
                             static_cast<float>(ATSC3_Q_SCALE * ATSC3_Q_SCALE));
    if (mag_sq < min_mag_sq || mag_sq < kMinMagnitudeSqDefault) {
        return sample_t(0, 0);
    }

    int32_t y_re = y.real();
    int32_t y_im = y.imag();

    // Y * conj(H) in Q30. Each product is up to ~2^30 in magnitude
    // (int16 x int16 promoted to int32), so their sum can exceed
    // INT32_MAX for strong subcarriers -- accumulate in int64_t.
    int64_t num_re = static_cast<int64_t>(y_re) * h_re + static_cast<int64_t>(y_im) * h_im;
    int64_t num_im = static_cast<int64_t>(y_im) * h_re - static_cast<int64_t>(y_re) * h_im;

    // Divide: result in Q15
    // Scale: (Q30 / Q30) needs to become Q15
    // So we compute (num << 15) / mag_sq, but to avoid overflow
    // we do num / (mag_sq >> 15)
    int32_t denom = mag_sq >> 15;
    if (denom == 0)
        denom = 1;

    int64_t out_re = num_re / denom;
    int64_t out_im = num_im / denom;

    // Clamp to Q15 range
    out_re = std::max<int64_t>(-32767, std::min<int64_t>(32767, out_re));
    out_im = std::max<int64_t>(-32767, std::min<int64_t>(32767, out_im));

    return sample_t(static_cast<int16_t>(out_re), static_cast<int16_t>(out_im));
#else
    float h_re = h.real();
    float h_im = h.imag();
    float mag_sq = h_re * h_re + h_im * h_im;

    // Check for very small channel
    float min_mag_sq = config_.min_channel_magnitude * config_.min_channel_magnitude;
    if (mag_sq < min_mag_sq || mag_sq < kMinMagnitudeSqDefault) {
        return sample_t(0.0f, 0.0f);
    }

    // Y * conj(H) / |H|^2
    float out_re = (y.real() * h_re + y.imag() * h_im) / mag_sq;
    float out_im = (y.imag() * h_re - y.real() * h_im) / mag_sq;

    return sample_t(out_re, out_im);
#endif
}

sample_t Equalizer::equalize_mmse(sample_t y, sample_t h) const {
    // MMSE: X_hat = Y * conj(H) / (|H|^2 + σ²_n)
    // The noise variance regularizes the denominator to reduce noise enhancement

#ifdef ATSC3_FIXED_POINT
    int32_t h_re = h.real();
    int32_t h_im = h.imag();

    // |H|^2 in Q30
    int32_t mag_sq = h_re * h_re + h_im * h_im;

    // Add noise variance (convert to Q30 equivalent)
    // noise_variance is in float, scale to Q30
    int32_t noise_q30 = static_cast<int32_t>(config_.noise_variance *
                                             static_cast<float>(ATSC3_Q_SCALE * ATSC3_Q_SCALE));
    int32_t denom_q30 = mag_sq + noise_q30;

    // Check for very small denominator
    if (denom_q30 < kMinMagnitudeSqDefault) {
        return sample_t(0, 0);
    }

    int32_t y_re = y.real();
    int32_t y_im = y.imag();

    // Y * conj(H) in Q30. Same int64_t widening as equalize_zf -- each
    // product is up to ~2^30, and their sum can exceed INT32_MAX.
    int64_t num_re = static_cast<int64_t>(y_re) * h_re + static_cast<int64_t>(y_im) * h_im;
    int64_t num_im = static_cast<int64_t>(y_im) * h_re - static_cast<int64_t>(y_re) * h_im;

    // Divide: (Q30 / Q30) → need Q15
    int32_t denom = denom_q30 >> 15;
    if (denom == 0)
        denom = 1;

    int64_t out_re = num_re / denom;
    int64_t out_im = num_im / denom;

    // Clamp
    out_re = std::max<int64_t>(-32767, std::min<int64_t>(32767, out_re));
    out_im = std::max<int64_t>(-32767, std::min<int64_t>(32767, out_im));

    return sample_t(static_cast<int16_t>(out_re), static_cast<int16_t>(out_im));
#else
    float h_re = h.real();
    float h_im = h.imag();
    float mag_sq = h_re * h_re + h_im * h_im;

    // MMSE denominator: |H|^2 + σ²_n
    float denom = mag_sq + config_.noise_variance;

    // Check for very small denominator
    if (denom < kMinMagnitudeSqDefault) {
        return sample_t(0.0f, 0.0f);
    }

    // Y * conj(H) / (|H|^2 + σ²_n)
    float out_re = (y.real() * h_re + y.imag() * h_im) / denom;
    float out_im = (y.imag() * h_re - y.real() * h_im) / denom;

    return sample_t(out_re, out_im);
#endif
}

sample_t Equalizer::equalize_sample(sample_t y, sample_t h) const {
    switch (config_.mode) {
        case EqualizationMode::MMSE:
            return equalize_mmse(y, h);
        case EqualizationMode::ZERO_FORCING:
        default:
            return equalize_zf(y, h);
    }
}

EqualizationResult Equalizer::equalize(const sample_t* received,
                                       const std::vector<sample_t>& channel_estimate,
                                       size_t symbol_index) {
    EqualizationResult result;
    result.symbol_index = symbol_index;

    if (received == nullptr || channel_estimate.empty()) {
        result.is_valid = false;
        return result;
    }

    size_t num_carriers = std::min(config_.num_active_carriers, channel_estimate.size());
    result.symbols.resize(num_carriers);
    result.num_zeroed_carriers = 0;

    // Equalize each subcarrier
    for (size_t k = 0; k < num_carriers; ++k) {
        result.symbols[k] = equalize_sample(received[k], channel_estimate[k]);

        // Count zeroed carriers
#ifdef ATSC3_FIXED_POINT
        if (result.symbols[k].real() == 0 && result.symbols[k].imag() == 0) {
            ++result.num_zeroed_carriers;
        }
#else
        if (result.symbols[k].real() == 0.0f && result.symbols[k].imag() == 0.0f) {
            ++result.num_zeroed_carriers;
        }
#endif
    }

    result.residual_phase = residual_phase_;
    result.is_valid = true;
    return result;
}

EqualizationResult Equalizer::equalize_with_pilots(const sample_t* received,
                                                   const std::vector<sample_t>& channel_estimate,
                                                   const std::vector<ofdm::PilotSymbol>& pilots,
                                                   size_t symbol_index) {
    EqualizationResult result;
    result.symbol_index = symbol_index;

    if (received == nullptr || channel_estimate.empty()) {
        result.is_valid = false;
        return result;
    }

    size_t num_carriers = std::min(config_.num_active_carriers, channel_estimate.size());
    result.symbols.resize(num_carriers);
    result.num_zeroed_carriers = 0;

    // Estimate residual phase from continual pilots
    if (config_.enable_phase_tracking && !pilots.empty()) {
        float phase = estimate_residual_phase(received, channel_estimate, pilots);

        // IIR smoothing
        float alpha = config_.phase_tracking_alpha;
        residual_phase_ = alpha * phase + (1.0f - alpha) * residual_phase_;

        update_phase_phasor(residual_phase_);
    }

    // Equalize each subcarrier
    for (size_t k = 0; k < num_carriers; ++k) {
        result.symbols[k] = equalize_sample(received[k], channel_estimate[k]);

        // Count zeroed carriers
#ifdef ATSC3_FIXED_POINT
        if (result.symbols[k].real() == 0 && result.symbols[k].imag() == 0) {
            ++result.num_zeroed_carriers;
        }
#else
        if (result.symbols[k].real() == 0.0f && result.symbols[k].imag() == 0.0f) {
            ++result.num_zeroed_carriers;
        }
#endif
    }

    // Apply phase correction
    if (config_.enable_phase_tracking) {
        apply_phase_correction(result.symbols, -residual_phase_);
    }

    result.residual_phase = residual_phase_;
    result.is_valid = true;
    return result;
}

float Equalizer::estimate_residual_phase(const sample_t* received,
                                         const std::vector<sample_t>& channel_estimate,
                                         const std::vector<ofdm::PilotSymbol>& pilots) const {
    if (pilots.empty()) {
        return 0.0f;
    }

    // Estimate residual phase from continual pilots
    // Phase error = angle(equalized / reference)
    //
    // For BPSK pilots: reference is real-valued (±1)
    // Phase error ≈ atan2(equalized.imag * ref.real, equalized.real * ref.real)

    float sin_sum = 0.0f;
    float cos_sum = 0.0f;
    size_t count = 0;

    for (const auto& pilot : pilots) {
        // Only use continual pilots for phase tracking
        if (pilot.type != ofdm::PilotType::CONTINUAL) {
            continue;
        }

        // Get relative position
        size_t rel_pos = pilot.subcarrier_index - config_.first_active_carrier;
        if (rel_pos >= channel_estimate.size()) {
            continue;
        }

        // Equalize pilot
        sample_t eq = equalize_sample(received[rel_pos], channel_estimate[rel_pos]);

        // Phase error from reference
#ifdef ATSC3_FIXED_POINT
        float eq_re = q15_to_float(eq.real());
        float eq_im = q15_to_float(eq.imag());
        float ref_re = q15_to_float(pilot.reference_value.real());
#else
        float eq_re = eq.real();
        float eq_im = eq.imag();
        float ref_re = pilot.reference_value.real();
#endif

        // For BPSK: expected = ref_re (real-valued)
        // Error = equalized - expected
        // Phase = atan2(imag, real * sign(ref))
        float sign = (ref_re >= 0.0f) ? 1.0f : -1.0f;

        // Accumulate for circular mean
        float mag = std::sqrt(eq_re * eq_re + eq_im * eq_im);
        if (mag > 1e-6f) {
            sin_sum += eq_im * sign / mag;
            cos_sum += eq_re * sign / mag;
            ++count;
        }
    }

    if (count == 0) {
        return 0.0f;
    }

    // Circular mean
    return std::atan2(sin_sum, cos_sum);
}

void Equalizer::apply_phase_correction(std::vector<sample_t>& symbols, float phase) {
    if (std::abs(phase) < 1e-6f) {
        return;
    }

    // Compute rotation phasor
    float cos_p = std::cos(phase);
    float sin_p = std::sin(phase);

    for (auto& s : symbols) {
#ifdef ATSC3_FIXED_POINT
        float s_re = q15_to_float(s.real());
        float s_im = q15_to_float(s.imag());

        float new_re = s_re * cos_p - s_im * sin_p;
        float new_im = s_re * sin_p + s_im * cos_p;

        // A phase rotation preserves magnitude, so a near-full-scale
        // input can rotate to a magnitude above the Q1.15 boundary
        // (e.g. |s|=1.0 exactly can reach 1.414). float_to_q15() itself
        // saturates, so no separate post-hoc clamp is needed here --
        // the previous code cast to int16_t (wrapping on overflow) and
        // only clamped afterward, which clamped an already-wrapped value.
        int16_t out_re = float_to_q15(new_re);
        int16_t out_im = float_to_q15(new_im);

        s = sample_t(out_re, out_im);
#else
        float new_re = s.real() * cos_p - s.imag() * sin_p;
        float new_im = s.real() * sin_p + s.imag() * cos_p;
        s = sample_t(new_re, new_im);
#endif
    }
}

void Equalizer::update_phase_phasor(float phase) {
    float cos_p = std::cos(-phase);
    float sin_p = std::sin(-phase);

#ifdef ATSC3_FIXED_POINT
    phase_phasor_ = sample_t(float_to_q15(cos_p), float_to_q15(sin_p));
#else
    phase_phasor_ = sample_t(cos_p, sin_p);
#endif
}

float compute_evm_db(const std::vector<sample_t>& equalized,
                     const std::vector<sample_t>& reference) {
    if (equalized.size() != reference.size() || equalized.empty()) {
        return 0.0f;
    }

    float error_power = 0.0f;
    float signal_power = 0.0f;

    for (size_t i = 0; i < equalized.size(); ++i) {
#ifdef ATSC3_FIXED_POINT
        float eq_re = q15_to_float(equalized[i].real());
        float eq_im = q15_to_float(equalized[i].imag());
        float ref_re = q15_to_float(reference[i].real());
        float ref_im = q15_to_float(reference[i].imag());
#else
        float eq_re = equalized[i].real();
        float eq_im = equalized[i].imag();
        float ref_re = reference[i].real();
        float ref_im = reference[i].imag();
#endif

        float err_re = eq_re - ref_re;
        float err_im = eq_im - ref_im;

        error_power += err_re * err_re + err_im * err_im;
        signal_power += ref_re * ref_re + ref_im * ref_im;
    }

    if (signal_power < 1e-10f) {
        return -100.0f;  // Undefined
    }

    // EVM = sqrt(error_power / signal_power)
    // EVM_dB = 20 * log10(EVM) = 10 * log10(error_power / signal_power)
    return 10.0f * std::log10(error_power / signal_power);
}

}  // namespace channel
}  // namespace atsc3
