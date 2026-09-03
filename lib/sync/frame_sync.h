#pragma once

// Frame Synchronization — Superframe and subframe boundary detection
//
// Tracks ATSC 3.0 frame boundaries using preamble correlation.
// Works with bootstrap detector output to maintain frame alignment.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32, TVALID, TREADY (frequency-corrected OFDM symbols)
//   Output: TDATA=cf32, TVALID, TREADY, TLAST (frame boundary marker)
//
// Reference: ATSC A/322 Section 5 (Bootstrap), Section 6 (Frame Structure)

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace sync {

// Frame sync state
enum class FrameSyncState {
    SEARCHING,  // Looking for bootstrap/preamble
    ACQUIRING,  // Found candidate, confirming
    LOCKED,     // Tracking frame boundaries
    HOLDOVER    // Lost lock, using last known timing
};

// Frame structure parameters (from L1 signaling or defaults)
struct FrameParams {
    // FFT size for data symbols
    size_t fft_size = 8192;

    // Cyclic prefix length (samples)
    size_t cp_length = 512;  // 1/16 of 8K

    // Number of OFDM symbols per subframe
    size_t symbols_per_subframe = 1;

    // Number of subframes per frame
    size_t subframes_per_frame = 1;

    // Preamble symbol count
    size_t preamble_symbols = 1;

    // Total samples per symbol (FFT + CP)
    size_t samples_per_symbol() const {
        return fft_size + cp_length;
    }

    // Total samples per subframe
    size_t samples_per_subframe() const {
        return symbols_per_subframe * samples_per_symbol();
    }

    // Total samples per frame (excluding preamble)
    size_t samples_per_frame() const {
        return subframes_per_frame * samples_per_subframe();
    }
};

// Frame sync configuration
struct FrameSyncConfig {
    // Sample rate in Hz
    double sample_rate_hz = 6.25e6;

    // Default frame parameters (updated after L1 decode)
    FrameParams frame_params;

    // Bootstrap correlation threshold for detection
    double detection_threshold = 0.7;

    // Lock acquisition: require N consecutive detections
    size_t lock_acquire_count = 3;

    // Lock loss: tolerate N consecutive misses before holdover
    size_t lock_loss_count = 5;

    // Holdover duration (frames) before returning to search
    size_t holdover_frames = 10;

    // Search window size (samples) around expected boundary
    size_t search_window = 64;
};

// Frame boundary event
struct FrameEvent {
    enum Type {
        FRAME_START,     // Start of frame (preamble)
        SUBFRAME_START,  // Start of subframe
        SYMBOL_START     // Start of data symbol
    };

    Type type;
    size_t sample_index;     // Global sample index
    size_t frame_number;     // Frame counter
    size_t subframe_number;  // Subframe within frame
    size_t symbol_number;    // Symbol within subframe
    double confidence;       // Detection confidence [0, 1]
};

// Frame Synchronizer
//
// Tracks frame boundaries using bootstrap/preamble correlation.
// Maintains lock across frames and handles brief signal drops.
//
// Usage:
//   FrameSyncConfig config;
//   FrameSync sync(config);
//
//   sync.set_frame_callback([](const FrameEvent& ev) {
//       // Handle frame/subframe/symbol boundary
//   });
//
//   // Feed from bootstrap detector or directly
//   sync.set_bootstrap_offset(bootstrap_sample_index);
//   sync.process(symbols, n_symbols);
//
class FrameSync {
public:
    using FrameCallback = std::function<void(const FrameEvent& event)>;

    explicit FrameSync(const FrameSyncConfig& config = FrameSyncConfig());
    ~FrameSync();

    // Process OFDM symbols (after FFT)
    void process(const sample_t* symbols, size_t n);

    // Notify bootstrap detection (coarse timing)
    void set_bootstrap_offset(size_t sample_index, double confidence = 1.0);

    // Set callback for frame events
    void set_frame_callback(FrameCallback cb) {
        frame_callback_ = std::move(cb);
    }

    // Update frame parameters (from L1 decode)
    void update_frame_params(const FrameParams& params);

    // Reset synchronizer state
    void reset();

    // Get current state
    FrameSyncState get_state() const {
        return state_;
    }

    // Get current frame number
    size_t get_frame_number() const {
        return frame_number_;
    }

    // Get current symbol position within frame
    size_t get_symbol_position() const {
        return symbol_position_;
    }

    // Get samples until next expected boundary
    size_t get_samples_to_boundary() const;

    // Get lock confidence
    double get_confidence() const {
        return confidence_;
    }

    // Is locked?
    bool is_locked() const {
        return state_ == FrameSyncState::LOCKED;
    }

    // Get configuration
    const FrameSyncConfig& get_config() const {
        return config_;
    }

    // Normalized correlation of symbols[offset .. offset+preamble_len)
    // against the preamble reference, in [0, 1]. Exposed (forwards to the
    // private implementation) for monitoring and for the Phase 9.0b
    // fixed-point-vs-reference equivalence test (test_frame_sync.cc) --
    // this computation is memoryless (no dependence on prior calls or
    // FSM state), so it can be compared call-by-call against a double
    // reference without the closed-loop trajectory concerns
    // TimingRecovery's equivalence test ran into.
    double compute_correlation(const sample_t* symbols, size_t n, size_t offset) {
        return correlate_preamble(symbols, n, offset);
    }

private:
    FrameSyncConfig config_;
    FrameSyncState state_;

    // Frame tracking
    size_t frame_number_;
    size_t subframe_number_;
    size_t symbol_position_;  // Symbol within current subframe
    size_t sample_counter_;   // Global sample counter

    // Lock tracking
    size_t consecutive_hits_;    // Consecutive successful detections
    size_t consecutive_misses_;  // Consecutive missed detections
    size_t holdover_counter_;    // Frames in holdover

    // Expected boundary
    size_t expected_boundary_;  // Next expected frame boundary
    double confidence_;         // Current lock confidence

    // Preamble reference for correlation
    std::vector<sample_t> preamble_ref_;

    // Callback
    FrameCallback frame_callback_;

    // State machine
    void transition_state(FrameSyncState new_state);
    void handle_searching(const sample_t* symbols, size_t n);
    void handle_acquiring(const sample_t* symbols, size_t n);
    void handle_locked(const sample_t* symbols, size_t n);
    void handle_holdover(const sample_t* symbols, size_t n);

    // Correlation. Return type stays double regardless of build mode
    // (this class's existing public/internal contract -- the state
    // machine above compares this against config_.detection_threshold,
    // a double); under ATSC3_FIXED_POINT the computation itself is
    // genuine fixed-point, converted only at this one boundary (Phase
    // 9.0b rewrite, see frame_sync.cc).
    double correlate_preamble(const sample_t* symbols, size_t n, size_t offset);

    // Event emission
    void emit_frame_event(FrameEvent::Type type);

    // Initialize preamble reference
    void init_preamble_reference();
};

}  // namespace sync
}  // namespace atsc3
