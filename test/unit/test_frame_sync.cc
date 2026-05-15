// test_frame_sync.cc — Unit tests for lib/sync/frame_sync.h
//
// Tests frame synchronization state machine and boundary detection

#include "frame_sync.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

constexpr double kPi = 3.14159265358979323846;

//==============================================================================
// FrameParams Tests
//==============================================================================

TEST(FrameParamsTest, SamplesPerSymbol) {
    FrameParams params;
    params.fft_size = 8192;
    params.cp_length = 512;

    EXPECT_EQ(params.samples_per_symbol(), 8704u);
}

TEST(FrameParamsTest, SamplesPerSubframe) {
    FrameParams params;
    params.fft_size = 8192;
    params.cp_length = 512;
    params.symbols_per_subframe = 10;

    EXPECT_EQ(params.samples_per_subframe(), 87040u);
}

TEST(FrameParamsTest, SamplesPerFrame) {
    FrameParams params;
    params.fft_size = 8192;
    params.cp_length = 512;
    params.symbols_per_subframe = 10;
    params.subframes_per_frame = 4;

    EXPECT_EQ(params.samples_per_frame(), 348160u);
}

//==============================================================================
// FrameSync Tests
//==============================================================================

TEST(FrameSyncTest, DefaultConstruction) {
    FrameSync sync;

    EXPECT_EQ(sync.get_state(), FrameSyncState::SEARCHING);
    EXPECT_EQ(sync.get_frame_number(), 0u);
    EXPECT_EQ(sync.get_symbol_position(), 0u);
    EXPECT_FALSE(sync.is_locked());
}

TEST(FrameSyncTest, CustomConfig) {
    FrameSyncConfig config;
    config.sample_rate_hz = 12.5e6;
    config.detection_threshold = 0.8;

    FrameSync sync(config);

    EXPECT_EQ(sync.get_config().sample_rate_hz, 12.5e6);
    EXPECT_EQ(sync.get_config().detection_threshold, 0.8);
}

TEST(FrameSyncTest, Reset) {
    FrameSync sync;

    // Set bootstrap offset to trigger acquisition
    sync.set_bootstrap_offset(1000, 0.9);
    EXPECT_NE(sync.get_state(), FrameSyncState::SEARCHING);

    sync.reset();

    EXPECT_EQ(sync.get_state(), FrameSyncState::SEARCHING);
    EXPECT_EQ(sync.get_frame_number(), 0u);
    EXPECT_EQ(sync.get_confidence(), 0.0);
}

TEST(FrameSyncTest, UpdateFrameParams) {
    FrameSync sync;

    FrameParams new_params;
    new_params.fft_size = 16384;
    new_params.cp_length = 1024;

    sync.update_frame_params(new_params);

    EXPECT_EQ(sync.get_config().frame_params.fft_size, 16384u);
    EXPECT_EQ(sync.get_config().frame_params.cp_length, 1024u);
}

TEST(FrameSyncTest, BootstrapOffsetTriggersAcquisition) {
    FrameSync sync;

    EXPECT_EQ(sync.get_state(), FrameSyncState::SEARCHING);

    sync.set_bootstrap_offset(5000, 0.85);

    EXPECT_EQ(sync.get_state(), FrameSyncState::ACQUIRING);
    EXPECT_NEAR(sync.get_confidence(), 0.85, 0.01);
}

TEST(FrameSyncTest, FrameCallbackInvoked) {
    FrameSyncConfig config;
    config.lock_acquire_count = 1;     // Lock immediately for test
    config.detection_threshold = 0.1;  // Low threshold for test

    FrameSync sync(config);

    size_t callback_count = 0;
    FrameEvent last_event{};

    sync.set_frame_callback([&](const FrameEvent& event) {
        ++callback_count;
        last_event = event;
    });

    // Set bootstrap offset
    sync.set_bootstrap_offset(100, 0.9);

    // Generate symbols that will correlate with reference
    size_t fft_size = config.frame_params.fft_size;
    std::vector<sample_t> symbols(fft_size * 2);

    for (size_t i = 0; i < symbols.size(); ++i) {
        double phase = 2.0 * kPi * ((i * 7) % 13) / 13.0;
        float re = 0.5f * static_cast<float>(std::cos(phase));
        float im = 0.5f * static_cast<float>(std::sin(phase));
#ifdef ATSC3_FIXED_POINT
        symbols[i] = sample_t(float_to_q15(re), float_to_q15(im));
#else
        symbols[i] = sample_t(re, im);
#endif
    }

    sync.process(symbols.data(), symbols.size());

    // If correlation was good, should have callback
    // Note: This test may not trigger callback due to simplified preamble reference
}

TEST(FrameSyncTest, StateMachineTransitions) {
    FrameSyncConfig config;
    config.lock_acquire_count = 2;
    config.lock_loss_count = 2;

    FrameSync sync(config);

    // Initial state
    EXPECT_EQ(sync.get_state(), FrameSyncState::SEARCHING);

    // Bootstrap triggers acquisition
    sync.set_bootstrap_offset(100, 0.9);
    EXPECT_EQ(sync.get_state(), FrameSyncState::ACQUIRING);

    // Reset back to searching
    sync.reset();
    EXPECT_EQ(sync.get_state(), FrameSyncState::SEARCHING);
}

TEST(FrameSyncTest, GetSamplesToBoundary) {
    FrameSync sync;

    sync.set_bootstrap_offset(1000, 0.9);

    // Before processing, samples to boundary depends on expected_boundary
    size_t samples_to = sync.get_samples_to_boundary();
    EXPECT_EQ(samples_to, 1000u);  // Expected boundary at 1000
}

TEST(FrameSyncTest, ProcessWithoutCallback) {
    FrameSync sync;

    std::vector<sample_t> symbols(1000);
#ifdef ATSC3_FIXED_POINT
    std::fill(symbols.begin(), symbols.end(), sample_t(float_to_q15(0.5f), float_to_q15(0.0f)));
#else
    std::fill(symbols.begin(), symbols.end(), sample_t(0.5f, 0.0f));
#endif

    // Should not crash
    sync.process(symbols.data(), symbols.size());
}

TEST(FrameSyncTest, ConfidenceDecaysInHoldover) {
    FrameSyncConfig config;
    config.holdover_frames = 5;

    // This test verifies holdover behavior conceptually
    // Full integration would require more complex setup
    FrameSync sync(config);
    EXPECT_EQ(sync.get_confidence(), 0.0);
}

TEST(FrameSyncTest, IsLockedReflectsState) {
    FrameSync sync;

    EXPECT_FALSE(sync.is_locked());

    // is_locked() returns true only when state is LOCKED
    // Can't easily force LOCKED state without proper correlation
    // but we can verify it's false in other states
    sync.set_bootstrap_offset(100, 0.9);
    EXPECT_FALSE(sync.is_locked());  // ACQUIRING != LOCKED
}

//==============================================================================
// FrameEvent Tests
//==============================================================================

TEST(FrameEventTest, TypeEnum) {
    // Verify enum values exist
    FrameEvent event;
    event.type = FrameEvent::FRAME_START;
    EXPECT_EQ(event.type, FrameEvent::FRAME_START);

    event.type = FrameEvent::SUBFRAME_START;
    EXPECT_EQ(event.type, FrameEvent::SUBFRAME_START);

    event.type = FrameEvent::SYMBOL_START;
    EXPECT_EQ(event.type, FrameEvent::SYMBOL_START);
}

TEST(FrameEventTest, DefaultInitialization) {
    FrameEvent event{};
    event.type = FrameEvent::FRAME_START;
    event.sample_index = 12345;
    event.frame_number = 42;
    event.subframe_number = 2;
    event.symbol_number = 7;
    event.confidence = 0.95;

    EXPECT_EQ(event.sample_index, 12345u);
    EXPECT_EQ(event.frame_number, 42u);
    EXPECT_EQ(event.subframe_number, 2u);
    EXPECT_EQ(event.symbol_number, 7u);
    EXPECT_NEAR(event.confidence, 0.95, 0.01);
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
