// signal_strength.h — Signal Strength Measurement
//
// dBm calculation from HAL RSSI + AGC gain correction

#pragma once

#include <cstddef>
#include <cstdint>

namespace atsc3 {
namespace metrics {

/// Configuration for signal strength measurement
struct SignalStrengthConfig {
    double agc_gain_offset_db = 0.0;   ///< Fixed offset to apply to RSSI
    double rssi_calibration_db = 0.0;  ///< Calibration offset for RSSI accuracy
    double min_rssi_dbm = -120.0;      ///< Minimum valid RSSI
    double max_rssi_dbm = 0.0;         ///< Maximum valid RSSI
    size_t averaging_samples = 16;     ///< Number of samples for smoothing
};

/// Signal strength statistics
struct SignalStrengthStats {
    double rssi_dbm = -120.0;            ///< Current RSSI in dBm
    double rssi_dbm_avg = -120.0;        ///< Averaged RSSI
    double rssi_dbm_peak = -120.0;       ///< Peak RSSI seen
    double rssi_dbm_min = 0.0;           ///< Minimum RSSI seen
    double agc_gain_db = 0.0;            ///< Current AGC gain
    double corrected_rssi_dbm = -120.0;  ///< RSSI corrected for AGC gain
    uint64_t samples_processed = 0;      ///< Total samples processed
};

/// Signal strength measurement with AGC correction
class SignalStrength {
public:
    SignalStrength();
    explicit SignalStrength(const SignalStrengthConfig& config);
    ~SignalStrength();

    // Non-copyable due to raw pointer
    SignalStrength(const SignalStrength&) = delete;
    SignalStrength& operator=(const SignalStrength&) = delete;

    /// Reset measurement state
    void reset();

    /// Update with new RSSI reading from HAL
    /// @param rssi_dbm Raw RSSI value in dBm
    void update_rssi(double rssi_dbm);

    /// Update AGC gain for correction
    /// @param gain_db Current AGC gain in dB
    void update_agc_gain(double gain_db);

    /// Get current RSSI in dBm (raw, not corrected)
    double get_rssi_dbm() const;

    /// Get AGC-corrected RSSI in dBm
    double get_corrected_rssi_dbm() const;

    /// Get detailed statistics
    SignalStrengthStats get_stats() const;

    /// Check if measurement is valid
    bool is_valid() const;

private:
    SignalStrengthConfig config_;

    // Running average
    double rssi_sum_ = 0.0;
    double* rssi_window_ = nullptr;
    size_t window_index_ = 0;
    bool window_filled_ = false;

    // Current state
    double current_rssi_dbm_ = -120.0;
    double current_agc_gain_db_ = 0.0;

    // Statistics
    SignalStrengthStats stats_;
};

}  // namespace metrics
}  // namespace atsc3
