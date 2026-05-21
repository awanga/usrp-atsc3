// mer_estimator.cc — MER Estimation implementation

#include "mer_estimator.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace metrics {

MerEstimator::MerEstimator() : MerEstimator(MerEstimatorConfig{}) {}

MerEstimator::MerEstimator(const MerEstimatorConfig& config) : config_(config) {
    error_window_.resize(config_.averaging_window, 0.0);
    ref_window_.resize(config_.averaging_window, 0.0);
    reset();
}

void MerEstimator::reset() {
    error_power_sum_ = 0.0;
    reference_power_sum_ = 0.0;
    sample_count_ = 0;
    window_index_ = 0;
    window_filled_ = false;
    std::fill(error_window_.begin(), error_window_.end(), 0.0);
    std::fill(ref_window_.begin(), ref_window_.end(), 0.0);
    stats_ = MerStats{};
}

void MerEstimator::set_constellation(ConstellationType type) {
    constellation_type_ = type;
    update_constellation_params();
}

void MerEstimator::update_constellation_params() {
    switch (constellation_type_) {
        case ConstellationType::QPSK:
            constellation_order_ = 2;
            constellation_scale_ = 1.0 / std::sqrt(2.0);
            break;
        case ConstellationType::QAM16:
            constellation_order_ = 4;
            constellation_scale_ = 1.0 / std::sqrt(10.0);
            break;
        case ConstellationType::QAM64:
            constellation_order_ = 8;
            constellation_scale_ = 1.0 / std::sqrt(42.0);
            break;
        case ConstellationType::QAM256:
            constellation_order_ = 16;
            constellation_scale_ = 1.0 / std::sqrt(170.0);
            break;
        case ConstellationType::QAM1024:
            constellation_order_ = 32;
            constellation_scale_ = 1.0 / std::sqrt(682.0);
            break;
        case ConstellationType::QAM4096:
            constellation_order_ = 64;
            constellation_scale_ = 1.0 / std::sqrt(2730.0);
            break;
    }
}

ATSC3_SAMPLE_T MerEstimator::hard_decision(const ATSC3_SAMPLE_T& symbol) const {
    // Normalize symbol
    float real_norm = symbol.real() / constellation_scale_;
    float imag_norm = symbol.imag() / constellation_scale_;

    // Quantize to nearest constellation point
    // For M-QAM, points are at odd integers: ..., -3, -1, 1, 3, ...
    int max_level = constellation_order_ - 1;

    auto quantize = [max_level](float val) -> float {
        // Round to nearest odd integer
        int level = static_cast<int>(std::round((val + max_level) / 2.0)) * 2 - max_level;
        // Clamp to valid range
        level = std::clamp(level, -max_level, max_level);
        return static_cast<float>(level);
    };

    float real_dec = quantize(real_norm) * constellation_scale_;
    float imag_dec = quantize(imag_norm) * constellation_scale_;

    return ATSC3_SAMPLE_T(real_dec, imag_dec);
}

void MerEstimator::process_symbols(const ATSC3_SAMPLE_T* symbols, size_t num_symbols) {
    if (symbols == nullptr || num_symbols == 0) {
        return;
    }

    double batch_error_power = 0.0;
    double batch_ref_power = 0.0;

    for (size_t i = 0; i < num_symbols; ++i) {
        // Make hard decision
        ATSC3_SAMPLE_T ideal = hard_decision(symbols[i]);

        // Calculate error
        ATSC3_SAMPLE_T error = symbols[i] - ideal;

        batch_error_power += std::norm(error);
        batch_ref_power += std::norm(ideal);
    }

    // Update sliding window
    if (window_filled_) {
        error_power_sum_ -= error_window_[window_index_];
        reference_power_sum_ -= ref_window_[window_index_];
    }

    error_window_[window_index_] = batch_error_power;
    ref_window_[window_index_] = batch_ref_power;
    error_power_sum_ += batch_error_power;
    reference_power_sum_ += batch_ref_power;

    window_index_ = (window_index_ + 1) % config_.averaging_window;
    if (window_index_ == 0) {
        window_filled_ = true;
    }

    sample_count_++;
    stats_.symbols_processed += num_symbols;

    // Update statistics
    if (reference_power_sum_ > 1e-15 && error_power_sum_ > 1e-15) {
        double mer_linear = reference_power_sum_ / error_power_sum_;
        stats_.mer_db = 10.0 * std::log10(mer_linear);
        stats_.mer_db = std::clamp(stats_.mer_db, config_.min_mer_db, config_.max_mer_db);

        // EVM = sqrt(error_power / ref_power) * 100%
        stats_.evm_percent = std::sqrt(error_power_sum_ / reference_power_sum_) * 100.0;

        size_t active_size = window_filled_ ? config_.averaging_window : window_index_;
        if (active_size > 0) {
            stats_.rms_error = std::sqrt(error_power_sum_ / static_cast<double>(active_size));
            stats_.reference_power = reference_power_sum_ / static_cast<double>(active_size);
        }
    } else if (error_power_sum_ <= 1e-15 && reference_power_sum_ > 0) {
        stats_.mer_db = config_.max_mer_db;
        stats_.evm_percent = 0.0;
    }
}

double MerEstimator::get_mer_db() const {
    return stats_.mer_db;
}

double MerEstimator::get_evm_percent() const {
    return stats_.evm_percent;
}

MerStats MerEstimator::get_stats() const {
    return stats_;
}

bool MerEstimator::is_valid() const {
    return sample_count_ >= config_.averaging_window / 2;
}

}  // namespace metrics
}  // namespace atsc3
