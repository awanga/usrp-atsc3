// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Frame Synchronization — frame boundary tracking from bootstraps
//
// Every ATSC 3.0 frame begins with a bootstrap (A/322 Section 7.2), so the
// frame boundaries are the starts of the validated bootstraps
// (BootstrapDecoder) and nothing else needs correlating. FrameSync checks
// each one against what the previous one promised and predicts the next:
//   - min_time_to_next (A/321 Section 6.1.1.1) puts the next bootstrap of
//     the same version in the window [T(X), T(X + 1)) ms after this one;
//   - for a time-aligned frame (L1B_frame_length_mode = 0, A/322 Section
//     9.2), L1B_frame_length gives the frame length exactly, bootstrap
//     included, so the next frame starts at this bootstrap + that length
//     (set_frame_length()). A symbol-aligned frame's length needs the
//     whole L1-Basic/L1-Detail symbol layout; until that is wired in only
//     the min_time_to_next window is used.
//
// All sample indices are at the bootstrap rate, 6.144 MS/s (6144 samples
// per ms). The windows are widened by the receiver's sample-clock error
// (max_clock_error_ppm of the interval) plus timing_slack_samples for the
// bootstrap timing estimate; those two are receiver assumptions, not
// spec values. Every frame is assumed to be an A/322 (bootstrap version
// 0.0) frame, so the next frame's bootstrap is the next one decoded.
//
// States:
//   SEARCHING  no bootstrap yet (or lost)
//   ACQUIRING  one bootstrap, waiting for a second that fits its window
//   LOCKED     the last bootstrap fitted the window of the one before
//   HOLDOVER   locked with a known frame length, but the next bootstrap
//              was missed: boundaries are predicted (FrameEvent::predicted)
//              for up to holdover_frames frames, then SEARCHING
// While LOCKED, a bootstrap that comes too early for the window is
// rejected as spurious (the lock rests on two consistent bootstraps);
// in ACQUIRING the new one replaces the old. Without a frame length a
// missed bootstrap cannot be predicted, so LOCKED drops to SEARCHING.
//
// Time advances by mark_searched(end): the caller declares that every
// bootstrap starting before end has been reported. This keeps the
// detector/decoder latency out of FrameSync.
//
// Interface: control-rate events (one per bootstrap), not a sample
// stream; no AXI4-S data path.
//
// Reference: ATSC A/321:2026 Section 6.1.1.1; A/322:2026 Sections 7.2,
// 9.2

#include "sync/bootstrap_signal.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace atsc3 {
namespace sync {

enum class FrameSyncState { SEARCHING, ACQUIRING, LOCKED, HOLDOVER };

struct FrameSyncConfig {
    // Sample-clock error tolerated between transmitter and receiver
    // (both oscillators), in parts per million of the interval.
    uint32_t max_clock_error_ppm = 100;

    // Bootstrap start timing error tolerated, in samples.
    uint32_t timing_slack_samples = 16;

    // Frames predicted in HOLDOVER before giving up.
    uint32_t holdover_frames = 3;
};

struct FrameEvent {
    size_t sample_index = 0;  // frame start (bootstrap symbol 0)
    size_t frame_number = 0;  // counted from acquisition
    bool predicted = false;   // HOLDOVER prediction, no bootstrap seen
};

class FrameSync {
public:
    using FrameCallback = std::function<void(const FrameEvent& event)>;

    static constexpr size_t kSamplesPerMs = 6144;  // at 6.144 MS/s

    explicit FrameSync(const FrameSyncConfig& config = FrameSyncConfig());

    void set_frame_callback(FrameCallback cb) {
        frame_callback_ = std::move(cb);
    }

    // A bootstrap that BootstrapDecoder validated, starting at
    // sample_index. Returns false when it was rejected (too early while
    // LOCKED). Emits a FrameEvent when accepted.
    bool on_bootstrap(size_t sample_index, const BootstrapSignaling& signaling);

    // Length of the frame that began with the last accepted bootstrap, in
    // samples (L1B_frame_length x 5 ms x 6144 for a time-aligned frame).
    // Applies to that frame only (the next accepted bootstrap clears it);
    // call again for each frame.
    void set_frame_length(size_t samples);

    // Every bootstrap starting before end has been reported.
    void mark_searched(size_t end);

    void reset();

    FrameSyncState get_state() const {
        return state_;
    }
    bool is_locked() const {
        return state_ == FrameSyncState::LOCKED;
    }
    size_t get_frame_number() const {
        return frame_number_;
    }
    // Start of the current frame (last accepted bootstrap or prediction).
    size_t get_frame_start() const {
        return frame_start_;
    }
    // Window the next bootstrap must start in: [earliest, latest].
    // Meaningless while SEARCHING.
    size_t get_window_earliest() const {
        return window_earliest_;
    }
    size_t get_window_latest() const {
        return window_latest_;
    }
    size_t get_rejected_count() const {
        return rejected_;
    }
    const FrameSyncConfig& get_config() const {
        return config_;
    }

private:
    size_t tolerance(size_t interval) const;
    void update_window();
    // continues: the bootstrap extends the current lock (frame number + 1)
    // rather than starting an acquisition (frame number 0).
    void accept(size_t sample_index, const BootstrapSignaling& signaling, bool continues);
    void emit(bool predicted);

    FrameSyncConfig config_;
    FrameCallback frame_callback_;

    FrameSyncState state_ = FrameSyncState::SEARCHING;
    size_t frame_number_ = 0;
    size_t frame_start_ = 0;
    size_t last_bootstrap_ = 0;      // start of the last accepted bootstrap
    unsigned min_time_to_next_ = 0;  // field of the last accepted bootstrap
    size_t frame_length_ = 0;        // 0 = unknown
    size_t window_earliest_ = 0;
    size_t window_latest_ = 0;
    uint32_t holdover_count_ = 0;
    size_t rejected_ = 0;
};

}  // namespace sync
}  // namespace atsc3
