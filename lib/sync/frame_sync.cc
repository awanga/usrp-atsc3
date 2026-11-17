// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_sync.cc — frame boundary tracking from validated bootstraps
//
// Reference: ATSC A/321:2026 Section 6.1.1.1; A/322:2026 Sections 7.2,
// 9.2

#include "frame_sync.h"

namespace atsc3 {
namespace sync {

FrameSync::FrameSync(const FrameSyncConfig& config) : config_(config) {}

void FrameSync::reset() {
    state_ = FrameSyncState::SEARCHING;
    frame_number_ = 0;
    frame_start_ = 0;
    last_bootstrap_ = 0;
    min_time_to_next_ = 0;
    frame_length_ = 0;
    window_earliest_ = 0;
    window_latest_ = 0;
    holdover_count_ = 0;
    rejected_ = 0;
}

size_t FrameSync::tolerance(size_t interval) const {
    uint64_t ppm = static_cast<uint64_t>(interval) * config_.max_clock_error_ppm;
    return static_cast<size_t>((ppm + 999999) / 1000000) + config_.timing_slack_samples;
}

// The window follows from the frame length when it is known, otherwise
// from min_time_to_next: [T(X), T(X + 1)) ms after the bootstrap (T(31) is
// the upper edge for X = 30). Tolerances grow with the time since the last
// bootstrap actually seen, which in HOLDOVER spans the predicted frames.
void FrameSync::update_window() {
    size_t lo = 0;
    size_t hi = 0;
    if (frame_length_ != 0) {
        lo = hi = frame_start_ + frame_length_;
    } else {
        lo = frame_start_ + bootstrap_min_time_to_next_ms(min_time_to_next_) * kSamplesPerMs;
        hi =
            frame_start_ + bootstrap_min_time_to_next_ms(min_time_to_next_ + 1) * kSamplesPerMs - 1;
    }
    size_t tol = tolerance(hi - last_bootstrap_);
    window_earliest_ = lo > tol ? lo - tol : 0;
    window_latest_ = hi + tol;
}

void FrameSync::emit(bool predicted) {
    if (frame_callback_) {
        FrameEvent ev;
        ev.sample_index = frame_start_;
        ev.frame_number = frame_number_;
        ev.predicted = predicted;
        frame_callback_(ev);
    }
}

void FrameSync::accept(size_t sample_index, const BootstrapSignaling& signaling, bool continues) {
    frame_number_ = continues ? frame_number_ + 1 : 0;
    frame_start_ = sample_index;
    last_bootstrap_ = sample_index;
    min_time_to_next_ = signaling.min_time_to_next;
    frame_length_ = 0;
    update_window();
    emit(false);
}

bool FrameSync::on_bootstrap(size_t sample_index, const BootstrapSignaling& signaling) {
    // A field value of 31 is not allowed (A/321 6.1.1.1); BootstrapDecoder
    // never validates it.
    if (signaling.min_time_to_next >= 31) {
        ++rejected_;
        return false;
    }
    if (state_ == FrameSyncState::SEARCHING) {
        state_ = FrameSyncState::ACQUIRING;
        accept(sample_index, signaling, false);
        return true;
    }
    // Bootstraps arrive in order; one at or before the current frame start
    // is a caller error.
    if (sample_index <= frame_start_) {
        ++rejected_;
        return false;
    }
    bool fits = sample_index >= window_earliest_ && sample_index <= window_latest_;
    if (fits) {
        state_ = FrameSyncState::LOCKED;
        accept(sample_index, signaling, true);
        return true;
    }
    if (state_ == FrameSyncState::LOCKED && sample_index < window_earliest_) {
        ++rejected_;
        return false;
    }
    state_ = FrameSyncState::ACQUIRING;
    accept(sample_index, signaling, false);
    return true;
}

void FrameSync::set_frame_length(size_t samples) {
    frame_length_ = samples;
    update_window();
}

void FrameSync::mark_searched(size_t end) {
    while (state_ != FrameSyncState::SEARCHING && end > window_latest_) {
        // Only a lock with a known frame length can be carried on.
        if (state_ == FrameSyncState::LOCKED && frame_length_ != 0) {
            state_ = FrameSyncState::HOLDOVER;
            holdover_count_ = 0;
        }
        if (state_ != FrameSyncState::HOLDOVER || holdover_count_ == config_.holdover_frames) {
            state_ = FrameSyncState::SEARCHING;
            return;
        }
        ++holdover_count_;
        ++frame_number_;
        frame_start_ += frame_length_;
        update_window();
        emit(true);
    }
}

}  // namespace sync
}  // namespace atsc3
