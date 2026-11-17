// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_frame_sync.cc — Unit tests for lib/sync/frame_sync.h
//
// Window edges come from the A/321 Section 6.1.1.1 mapping T(X) (its
// worked example: X = 10 puts the next bootstrap between 700 and 800 ms)
// and the A/322 Section 9.2 frame length (L1B_frame_length x 5 ms), at
// 6144 samples per ms. The clock/timing tolerances are FrameSync's own
// documented receiver assumptions; most tests set them to zero so the
// spec edges are checked exactly.

#include "sync/frame_sync.h"

#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

constexpr size_t kMs = 6144;  // samples per ms at 6.144 MS/s

BootstrapSignaling sig(unsigned min_time_to_next) {
    BootstrapSignaling s;
    s.min_time_to_next = static_cast<uint8_t>(min_time_to_next);
    s.bsr_coefficient = 2;
    return s;
}

FrameSyncConfig exact() {
    FrameSyncConfig c;
    c.max_clock_error_ppm = 0;
    c.timing_slack_samples = 0;
    return c;
}

struct Recorder {
    std::vector<FrameEvent> events;
    void attach(FrameSync& fs) {
        fs.set_frame_callback([this](const FrameEvent& e) { events.push_back(e); });
    }
};

TEST(FrameSyncTest, StartsSearchingWithoutEvents) {
    FrameSync fs;
    Recorder rec;
    rec.attach(fs);
    fs.mark_searched(100 * kMs);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
    EXPECT_FALSE(fs.is_locked());
    EXPECT_TRUE(rec.events.empty());
}

TEST(FrameSyncTest, FirstBootstrapStartsAcquisition) {
    FrameSync fs(exact());
    Recorder rec;
    rec.attach(fs);
    EXPECT_TRUE(fs.on_bootstrap(5000, sig(10)));
    EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING);
    ASSERT_EQ(rec.events.size(), 1u);
    EXPECT_EQ(rec.events[0].sample_index, 5000u);
    EXPECT_EQ(rec.events[0].frame_number, 0u);
    EXPECT_FALSE(rec.events[0].predicted);
    // A/321 example: X = 10 -> [700, 800) ms.
    EXPECT_EQ(fs.get_window_earliest(), 5000 + 700 * kMs);
    EXPECT_EQ(fs.get_window_latest(), 5000 + 800 * kMs - 1);
}

// X = 10: the next bootstrap starts in [700, 800) ms.
TEST(FrameSyncTest, MinTimeWindowEdgesLock) {
    const size_t t0 = 1000;
    for (size_t at : {t0 + 700 * kMs, t0 + 800 * kMs - 1}) {
        FrameSync fs(exact());
        fs.on_bootstrap(t0, sig(10));
        EXPECT_TRUE(fs.on_bootstrap(at, sig(10)));
        EXPECT_EQ(fs.get_state(), FrameSyncState::LOCKED) << at - t0;
        EXPECT_EQ(fs.get_frame_number(), 1u);
        EXPECT_EQ(fs.get_frame_start(), at);
    }
    for (size_t at : {t0 + 700 * kMs - 1, t0 + 800 * kMs}) {
        FrameSync fs(exact());
        fs.on_bootstrap(t0, sig(10));
        EXPECT_TRUE(fs.on_bootstrap(at, sig(10)));
        EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING) << at - t0;
        EXPECT_EQ(fs.get_frame_number(), 0u);
        EXPECT_EQ(fs.get_frame_start(), at);
    }
}

// Each piece of the T(X) mapping: X = 0, 7, 8, 15, 16, 23, 24, 30.
TEST(FrameSyncTest, MinTimeWindowFollowsEveryMappingSegment) {
    struct Row {
        unsigned x;
        size_t lo_ms, hi_ms;
    };
    const Row rows[] = {{0, 50, 100},     {7, 400, 500},    {8, 500, 600},    {15, 1200, 1300},
                        {16, 1300, 1500}, {23, 2700, 2900}, {24, 2900, 3300}, {30, 5300, 5700}};
    for (const Row& r : rows) {
        FrameSync fs(exact());
        fs.on_bootstrap(0 + 1, sig(r.x));
        EXPECT_EQ(fs.get_window_earliest(), 1 + r.lo_ms * kMs) << "X=" << r.x;
        EXPECT_EQ(fs.get_window_latest(), r.hi_ms * kMs) << "X=" << r.x;
    }
}

TEST(FrameSyncTest, ToleranceWidensTheWindow) {
    FrameSyncConfig c;
    c.max_clock_error_ppm = 100;
    c.timing_slack_samples = 16;
    FrameSync fs(c);
    fs.on_bootstrap(0, sig(10));
    // The window ends 800 ms - 1 sample after the bootstrap: 4915199
    // samples, x 100e-6 = 491.52 -> 492, + 16.
    const size_t tol = 492 + 16;
    EXPECT_EQ(fs.get_window_earliest(), 700 * kMs - tol);
    EXPECT_EQ(fs.get_window_latest(), 800 * kMs - 1 + tol);
}

TEST(FrameSyncTest, LockedRejectsEarlyBootstrap) {
    FrameSync fs(exact());
    Recorder rec;
    rec.attach(fs);
    fs.on_bootstrap(0, sig(10));
    fs.on_bootstrap(750 * kMs, sig(10));
    ASSERT_TRUE(fs.is_locked());
    EXPECT_FALSE(fs.on_bootstrap(750 * kMs + 699 * kMs, sig(10)));
    EXPECT_TRUE(fs.is_locked());
    EXPECT_EQ(fs.get_rejected_count(), 1u);
    EXPECT_EQ(rec.events.size(), 2u);
    EXPECT_TRUE(fs.on_bootstrap(750 * kMs + 760 * kMs, sig(10)));
    EXPECT_EQ(fs.get_frame_number(), 2u);
    EXPECT_EQ(rec.events.back().frame_number, 2u);
}

TEST(FrameSyncTest, LockedRestartsOnLateBootstrap) {
    FrameSync fs(exact());
    fs.on_bootstrap(0, sig(10));
    fs.on_bootstrap(750 * kMs, sig(10));
    EXPECT_TRUE(fs.on_bootstrap(750 * kMs + 800 * kMs, sig(10)));
    EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING);
    EXPECT_EQ(fs.get_frame_number(), 0u);
}

TEST(FrameSyncTest, AcquiringReplacesOnEarlyBootstrap) {
    FrameSync fs(exact());
    fs.on_bootstrap(0, sig(10));
    EXPECT_TRUE(fs.on_bootstrap(300 * kMs, sig(4)));
    EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING);
    EXPECT_EQ(fs.get_frame_start(), 300 * kMs);
    // X = 4 -> [250, 300) ms from the replacement.
    EXPECT_EQ(fs.get_window_earliest(), 550 * kMs);
    EXPECT_EQ(fs.get_rejected_count(), 0u);
}

// L1B_frame_length = 50 -> 250 ms, bootstrap included; X = 4 -> [250, 300).
TEST(FrameSyncTest, FrameLengthGivesTheExactBoundary) {
    const size_t len = 50 * 5 * kMs;
    {
        FrameSync fs(exact());
        fs.on_bootstrap(0, sig(4));
        fs.set_frame_length(len);
        EXPECT_EQ(fs.get_window_earliest(), len);
        EXPECT_EQ(fs.get_window_latest(), len);
        EXPECT_TRUE(fs.on_bootstrap(len, sig(4)));
        EXPECT_TRUE(fs.is_locked());
    }
    {
        // Inside the min_time_to_next window, but not at the frame end.
        FrameSync fs(exact());
        fs.on_bootstrap(0, sig(4));
        fs.set_frame_length(len);
        fs.on_bootstrap(len + 1, sig(4));
        EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING);
    }
    {
        // The length applies to the frame it was set for only.
        FrameSync fs(exact());
        fs.on_bootstrap(0, sig(4));
        fs.set_frame_length(len);
        fs.on_bootstrap(len, sig(4));
        EXPECT_EQ(fs.get_window_earliest(), len + 250 * kMs);
        EXPECT_EQ(fs.get_window_latest(), len + 300 * kMs - 1);
    }
}

TEST(FrameSyncTest, MarkSearchedDeadlineIsTheWindowEnd) {
    FrameSync fs(exact());
    fs.on_bootstrap(0, sig(10));
    fs.on_bootstrap(750 * kMs, sig(10));
    const size_t latest = fs.get_window_latest();
    fs.mark_searched(latest);  // a bootstrap at `latest` may still come
    EXPECT_TRUE(fs.is_locked());
    fs.mark_searched(latest + 1);
    // No frame length: nothing to predict from.
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
}

TEST(FrameSyncTest, AcquiringFallsBackToSearchingOnMiss) {
    FrameSync fs(exact());
    fs.on_bootstrap(0, sig(10));
    fs.mark_searched(800 * kMs);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);

    // Holdover needs a lock, even with the frame length known.
    Recorder rec;
    rec.attach(fs);
    fs.on_bootstrap(1000 * kMs, sig(4));
    fs.set_frame_length(250 * kMs);
    fs.mark_searched(1250 * kMs + 1);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
    EXPECT_EQ(rec.events.size(), 1u);
}

TEST(FrameSyncTest, HoldoverPredictsAndRelocks) {
    const size_t len = 250 * kMs;
    FrameSync fs(exact());
    Recorder rec;
    rec.attach(fs);
    fs.on_bootstrap(0, sig(4));
    fs.set_frame_length(len);
    fs.on_bootstrap(len, sig(4));
    fs.set_frame_length(len);
    ASSERT_TRUE(fs.is_locked());

    fs.mark_searched(2 * len + 1);
    EXPECT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    ASSERT_EQ(rec.events.size(), 3u);
    EXPECT_EQ(rec.events[2].sample_index, 2 * len);
    EXPECT_EQ(rec.events[2].frame_number, 2u);
    EXPECT_TRUE(rec.events[2].predicted);

    EXPECT_TRUE(fs.on_bootstrap(3 * len, sig(4)));
    EXPECT_TRUE(fs.is_locked());
    EXPECT_EQ(fs.get_frame_number(), 3u);
    EXPECT_FALSE(rec.events.back().predicted);
}

// The holdover budget is per loss of lock, not per acquisition.
TEST(FrameSyncTest, HoldoverBudgetRestartsAfterRelock) {
    const size_t len = 250 * kMs;
    FrameSyncConfig c = exact();
    c.holdover_frames = 2;
    FrameSync fs(c);
    fs.on_bootstrap(0, sig(4));
    fs.set_frame_length(len);
    fs.on_bootstrap(len, sig(4));
    fs.set_frame_length(len);
    fs.mark_searched(3 * len + 1);  // both predictions used
    ASSERT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    ASSERT_TRUE(fs.on_bootstrap(4 * len, sig(4)));
    fs.set_frame_length(len);
    fs.mark_searched(6 * len + 1);  // two more
    EXPECT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    fs.mark_searched(7 * len + 1);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
}

TEST(FrameSyncTest, HoldoverExpiresAfterConfiguredFrames) {
    const size_t len = 250 * kMs;
    FrameSyncConfig c = exact();
    c.holdover_frames = 2;
    FrameSync fs(c);
    Recorder rec;
    rec.attach(fs);
    fs.on_bootstrap(0, sig(4));
    fs.set_frame_length(len);
    fs.on_bootstrap(len, sig(4));
    fs.set_frame_length(len);

    fs.mark_searched(3 * len + 1);  // misses at 2 len and 3 len
    EXPECT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    // The bootstrap closing the 2nd predicted frame may still come.
    fs.mark_searched(4 * len);
    EXPECT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    fs.mark_searched(4 * len + 1);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
    ASSERT_EQ(rec.events.size(), 4u);
    EXPECT_EQ(rec.events[2].sample_index, 2 * len);
    EXPECT_EQ(rec.events[3].sample_index, 3 * len);
    EXPECT_TRUE(rec.events[3].predicted);
}

TEST(FrameSyncTest, HoldoverToleranceGrowsFromLastBootstrapSeen) {
    const size_t len = 250 * kMs;
    FrameSyncConfig c;
    c.max_clock_error_ppm = 100;
    c.timing_slack_samples = 0;
    FrameSync fs(c);
    fs.on_bootstrap(0, sig(4));
    fs.set_frame_length(len);
    // 1536000 x 100e-6 = 153.6 -> 154
    EXPECT_EQ(fs.get_window_latest(), len + 154);
    fs.on_bootstrap(len, sig(4));
    fs.set_frame_length(len);
    fs.mark_searched(2 * len + 155);
    ASSERT_EQ(fs.get_state(), FrameSyncState::HOLDOVER);
    // Predicted frame at 2 len; the next boundary is 2 len after the last
    // bootstrap seen: 3072000 x 100e-6 = 307.2 -> 308.
    EXPECT_EQ(fs.get_window_earliest(), 3 * len - 308);
    EXPECT_EQ(fs.get_window_latest(), 3 * len + 308);
}

TEST(FrameSyncTest, HoldoverDisabledDropsToSearching) {
    const size_t len = 250 * kMs;
    FrameSyncConfig c = exact();
    c.holdover_frames = 0;
    FrameSync fs(c);
    fs.on_bootstrap(0, sig(4));
    fs.set_frame_length(len);
    fs.on_bootstrap(len, sig(4));
    fs.set_frame_length(len);
    fs.mark_searched(2 * len + 1);
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
}

TEST(FrameSyncTest, RejectsReservedMinTimeAndOutOfOrder) {
    FrameSync fs(exact());
    EXPECT_FALSE(fs.on_bootstrap(0, sig(31)));
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
    fs.on_bootstrap(1000, sig(10));
    EXPECT_FALSE(fs.on_bootstrap(1000, sig(10)));
    EXPECT_FALSE(fs.on_bootstrap(999, sig(10)));
    EXPECT_EQ(fs.get_rejected_count(), 3u);
    EXPECT_EQ(fs.get_frame_start(), 1000u);
}

TEST(FrameSyncTest, ResetReturnsToSearching) {
    FrameSync fs(exact());
    fs.on_bootstrap(0, sig(10));
    fs.on_bootstrap(750 * kMs, sig(10));
    fs.on_bootstrap(760 * kMs, sig(10));  // rejected: too early
    ASSERT_EQ(fs.get_rejected_count(), 1u);
    fs.reset();
    EXPECT_EQ(fs.get_state(), FrameSyncState::SEARCHING);
    EXPECT_EQ(fs.get_frame_number(), 0u);
    EXPECT_EQ(fs.get_rejected_count(), 0u);
    fs.on_bootstrap(5, sig(10));
    EXPECT_EQ(fs.get_state(), FrameSyncState::ACQUIRING);
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
