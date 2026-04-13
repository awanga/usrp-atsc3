// agc.cc — AGC Controller implementation
//
// Power-feedback AGC with configurable attack/release time constants.

#include "agc.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace hal {

AgcController::AgcController(const AgcConfig& config)
    : config_(config),
      current_gain_db_(config.initial_gain_db),
      measured_power_dbfs_(-100.0) {
    update_coefficients();
}

double AgcController::process(const std::complex<float>* buf, size_t n) {
    if (n == 0) {
        return current_gain_db_;
    }

    // Measure input power
    measured_power_dbfs_ = compute_power_dbfs(buf, n);

    // Compute error (how far from target)
    // Positive error means signal too strong, need to reduce gain
    // Negative error means signal too weak, need to increase gain
    double error_db = measured_power_dbfs_ - config_.target_power_dbfs;

    // Apply deadband
    if (std::abs(error_db) < config_.deadband_db) {
        return current_gain_db_;
    }

    // Apply asymmetric filter (fast attack, slow release)
    double coeff = (error_db > 0.0) ? attack_coeff_ : release_coeff_;

    // Leaky integrator: gain -= coeff * error
    // If signal too strong (error > 0), reduce gain
    // If signal too weak (error < 0), increase gain (subtracting negative)
    double gain_delta = coeff * error_db;

    // Update gain
    double new_gain = current_gain_db_ - gain_delta;

    // Clamp to limits
    new_gain = std::clamp(new_gain, config_.min_gain_db, config_.max_gain_db);

    current_gain_db_ = new_gain;
    return current_gain_db_;
}

double AgcController::get_gain() const {
    return current_gain_db_;
}

double AgcController::get_power_dbfs() const {
    return measured_power_dbfs_;
}

void AgcController::reset() {
    current_gain_db_ = config_.initial_gain_db;
    measured_power_dbfs_ = -100.0;
}

void AgcController::set_config(const AgcConfig& config) {
    config_ = config;
    update_coefficients();
    // Clamp current gain to new limits
    current_gain_db_ =
        std::clamp(current_gain_db_, config_.min_gain_db, config_.max_gain_db);
}

const AgcConfig& AgcController::get_config() const {
    return config_;
}

void AgcController::update_coefficients() {
    // Convert time constants to filter coefficients
    // For a simple first-order IIR filter: y[n] = (1-alpha)*y[n-1] + alpha*x[n]
    // Time constant tau in samples: alpha = 1 - exp(-1/tau)
    // For small alpha, alpha ≈ 1/tau

    // Attack coefficient (larger = faster response)
    if (config_.attack_samples > 0.0) {
        attack_coeff_ = 1.0 - std::exp(-1.0 / config_.attack_samples);
    } else {
        attack_coeff_ = 1.0;  // Instant
    }

    // Release coefficient (smaller = slower response)
    if (config_.release_samples > 0.0) {
        release_coeff_ = 1.0 - std::exp(-1.0 / config_.release_samples);
    } else {
        release_coeff_ = 1.0;  // Instant
    }
}

double AgcController::compute_power_dbfs(const std::complex<float>* buf, size_t n) {
    if (n == 0) {
        return -120.0;  // Noise floor
    }

    // Compute average power |x|^2
    double power_sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        float re = buf[i].real();
        float im = buf[i].imag();
        power_sum += static_cast<double>(re * re + im * im);
    }
    double avg_power = power_sum / static_cast<double>(n);

    // Convert to dBFS
    // Full scale is |x| = 1.0, so power = 1.0 = 0 dBFS
    constexpr double kMinPower = 1e-20;  // Avoid log(0)
    if (avg_power < kMinPower) {
        return -120.0;
    }
    return 10.0 * std::log10(avg_power);
}

}  // namespace hal
}  // namespace atsc3
