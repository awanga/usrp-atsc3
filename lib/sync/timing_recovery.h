#pragma once

// Timing Recovery — Symbol timing synchronization for OFDM
//
// Implements Gardner timing error detector (TED) with polyphase interpolator
// for fractional sample delay adjustment.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (oversampled IQ samples)
//   Output: TDATA=cf32, TVALID, TREADY (symbol-rate samples, aligned)
//
// Reference: ATSC A/322 Section 6.1 (Timing)

#include "types.h"

#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

namespace atsc3 {
namespace sync {

// Polyphase filter bank configuration
struct PolyphaseConfig {
    // Number of filter phases (interpolation factor within symbol)
    size_t num_phases = 16;

    // Number of taps per phase
    size_t taps_per_phase = 32;

    // Oversampling factor of input signal (samples per symbol)
    size_t samples_per_symbol = 2;
};

// Timing recovery configuration
struct TimingRecoveryConfig {
    // Sample rate in Hz
    double sample_rate_hz = 6.25e6;

    // Symbol rate in Hz (FFT size * (1 + CP fraction) / sample_rate)
    // For 8K FFT with 1/16 CP: 8192 * (1 + 1/16) = 8704 samples/symbol
    double symbol_rate_hz = 718.75;  // ~6.25e6 / 8704

    // Loop bandwidth (Hz) - controls tracking speed vs. jitter
    double loop_bandwidth_hz = 1.0;

    // Loop damping factor (critically damped = 1.0)
    double loop_damping = 1.0;

    // Polyphase filter configuration
    PolyphaseConfig polyphase;

    // Initial timing offset estimate (fractional samples)
    double initial_offset = 0.0;
};

// Polyphase Interpolator
//
// Implements efficient fractional delay using a polyphase filter bank.
// Given N phases, can interpolate to 1/N sample resolution.
//
class PolyphaseInterpolator {
public:
    explicit PolyphaseInterpolator(const PolyphaseConfig& config = PolyphaseConfig());
    ~PolyphaseInterpolator();

    // Interpolate a single sample at fractional delay mu ∈ [0, 1)
    // buf: circular buffer of at least taps_per_phase samples
    // buf_idx: index of current sample in buffer
    // mu: fractional delay (0 = no delay, approaching 1 = almost 1 sample delay)
    sample_t interpolate(const sample_t* buf, size_t buf_idx, double mu) const;

    // Get configuration
    const PolyphaseConfig& get_config() const {
        return config_;
    }

    // Get total number of taps
    size_t get_total_taps() const {
        return config_.num_phases * config_.taps_per_phase;
    }

private:
    PolyphaseConfig config_;

    // Filter coefficients: [phase][tap]
    std::vector<std::vector<float>> coeffs_;

    // Initialize filter coefficients (raised cosine with rolloff)
    void init_coefficients();
};

// Gardner Timing Error Detector
//
// Computes timing error from midpoint samples using Gardner algorithm:
//   e[n] = Re{ (x[n] - x[n-1]) * conj(x[n-0.5]) }
//
// Works best with 2x oversampling.
//
class GardnerTed {
public:
    GardnerTed();
    ~GardnerTed();

    // Compute timing error from three samples
    // x_curr: current symbol sample
    // x_mid: midpoint sample (between x_prev and x_curr)
    // x_prev: previous symbol sample
    // Returns: timing error (positive = sample too early, negative = sample too late)
    double compute_error(sample_t x_curr, sample_t x_mid, sample_t x_prev) const;

    // Reset detector state
    void reset();

private:
    // Previous samples for streaming operation
    sample_t prev_symbol_;
    sample_t prev_mid_;
    bool have_prev_;
};

// Timing Recovery Loop
//
// Complete timing recovery system combining:
// - Gardner TED for error detection
// - Second-order loop filter for smoothing
// - Polyphase interpolator for sample adjustment
//
// Usage:
//   TimingRecoveryConfig config;
//   TimingRecovery timing(config);
//
//   // Set callback for output symbols
//   timing.set_symbol_callback([](const sample_t& sym, double mu) {
//       // Process symbol-rate sample at fractional offset mu
//   });
//
//   // Feed input samples (2x oversampled)
//   timing.process(input_buffer, n_samples);
//
class TimingRecovery {
public:
    using SymbolCallback = std::function<void(const sample_t& symbol, double mu)>;

    explicit TimingRecovery(const TimingRecoveryConfig& config = TimingRecoveryConfig());
    ~TimingRecovery();

    // Process input samples
    void process(const sample_t* in, size_t n);

    // Process single sample
    void process_sample(const sample_t& sample);

    // Set callback for output symbols
    void set_symbol_callback(SymbolCallback cb) {
        symbol_callback_ = std::move(cb);
    }

    // Reset timing recovery state
    void reset();

    // Get current timing estimate (fractional samples)
    double get_timing_offset() const {
        return mu_;
    }

    // Get current timing error (filtered)
    double get_timing_error() const {
        return timing_error_;
    }

    // Get symbol count
    size_t get_symbol_count() const {
        return symbol_count_;
    }

    // Get configuration
    const TimingRecoveryConfig& get_config() const {
        return config_;
    }

    // Set timing offset manually (for fast acquisition)
    void set_timing_offset(double mu);

    // Lock/unlock timing loop (for training vs. tracking modes)
    void set_locked(bool locked) {
        locked_ = locked;
    }
    bool is_locked() const {
        return locked_;
    }

private:
    TimingRecoveryConfig config_;

    // Components
    PolyphaseInterpolator interpolator_;
    GardnerTed ted_;

    // Sample buffer (circular)
    std::vector<sample_t> buffer_;
    size_t buf_write_idx_;
    size_t buf_read_idx_;
    size_t buf_count_;

    // Timing loop state
    double mu_;               // Fractional timing offset [0, 1)
    double timing_error_;     // Filtered timing error
    double loop_integrator_;  // Second-order loop integrator

    // Loop filter gains (computed from bandwidth and damping)
    double kp_;  // Proportional gain
    double ki_;  // Integral gain

    // Output state
    SymbolCallback symbol_callback_;
    size_t sample_count_;
    size_t symbol_count_;
    size_t samples_since_symbol_;

    // Lock state
    bool locked_;

    // Compute loop filter gains from config
    void compute_loop_gains();

    // Update timing loop with new error
    void update_loop(double error);

    // Emit interpolated symbol
    void emit_symbol();
};

}  // namespace sync
}  // namespace atsc3
