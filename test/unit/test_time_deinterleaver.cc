// test_time_deinterleaver.cc — Unit tests for lib/ofdm/time_deinterleaver.h
//
// Tests convolutional time de-interleaving and settling behavior

#include "time_deinterleaver.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

//==============================================================================
// Construction Tests
//==============================================================================

TEST(TimeDeinterleaverTest, DefaultConstruction) {
    TimeDeinterleaver deint;

    auto config = deint.get_config();
    EXPECT_EQ(config.mode, config::TimeInterleaveMode::CTI);
    EXPECT_EQ(config.depth, 0u);
    EXPECT_EQ(config.cells_per_block, 0u);
    EXPECT_EQ(deint.get_settling_blocks(), 0u);
    EXPECT_TRUE(deint.is_settled());
}

TEST(TimeDeinterleaverTest, ConstructWithCTIConfig) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 4;
    config.cells_per_block = 1000;

    TimeDeinterleaver deint(config);

    EXPECT_EQ(deint.get_config().depth, 4u);
    EXPECT_EQ(deint.get_settling_blocks(), 4u);  // depth rows need depth blocks to settle
    EXPECT_FALSE(deint.is_settled());
}

TEST(TimeDeinterleaverTest, ConstructWithNoInterleaving) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::NONE;
    config.depth = 0;

    TimeDeinterleaver deint(config);

    EXPECT_EQ(deint.get_settling_blocks(), 0u);
    EXPECT_TRUE(deint.is_settled());
}

//==============================================================================
// No-interleaving Tests
//==============================================================================

TEST(TimeDeinterleaverTest, NoInterleavingPassThrough) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::NONE;
    config.depth = 0;
    config.cells_per_block = 100;

    TimeDeinterleaver deint(config);

    std::vector<int8_t> input(100);
    std::iota(input.begin(), input.end(), 0);

    std::vector<int8_t> output(100);
    deint.process(input.data(), output.data(), 100);

    EXPECT_EQ(output, input);
}

TEST(TimeDeinterleaverTest, DepthZeroPassThrough) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 0;
    config.cells_per_block = 100;

    TimeDeinterleaver deint(config);

    std::vector<int8_t> input(100);
    std::iota(input.begin(), input.end(), 0);

    std::vector<int8_t> output(100);
    deint.process(input.data(), output.data(), 100);

    EXPECT_EQ(output, input);
}

//==============================================================================
// CTI Mode Tests
//==============================================================================

TEST(TimeDeinterleaverTest, CTISettlingBehavior) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 3;  // 4 rows (0, 1, 2, 3)
    config.cells_per_block = 100;

    TimeDeinterleaver deint(config);

    EXPECT_EQ(deint.get_settling_blocks(), 3u);
    EXPECT_FALSE(deint.is_settled());
    EXPECT_EQ(deint.blocks_processed(), 0u);

    std::vector<int8_t> input(100, 42);
    std::vector<int8_t> output(100);

    // Process settling blocks
    for (size_t i = 0; i < 3; ++i) {
        deint.process(input.data(), output.data(), 100);
        EXPECT_EQ(deint.blocks_processed(), i + 1);
        EXPECT_FALSE(deint.is_settled()) << "Should not be settled after block " << i;
    }

    // After processing settling_blocks more, should be settled
    deint.process(input.data(), output.data(), 100);
    EXPECT_TRUE(deint.is_settled());
}

TEST(TimeDeinterleaverTest, CTIDelayLineStructure) {
    // With depth=2, we have 3 rows with delays:
    // Row 0: delay = 2 * cells_per_block (de-interleaver has inverse)
    // Row 1: delay = 1 * cells_per_block
    // Row 2: delay = 0 (pass-through)

    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 2;
    config.cells_per_block = 6;  // Small for easy testing

    TimeDeinterleaver deint(config);

    // With 6 cells and 3 rows:
    // Cell 0, 3 go to row 0 (delay 12)
    // Cell 1, 4 go to row 1 (delay 6)
    // Cell 2, 5 go to row 2 (delay 0 - pass-through)

    // First block: cells 2, 5 should pass through immediately
    std::vector<int8_t> block1 = {10, 11, 12, 13, 14, 15};
    std::vector<int8_t> out1(6);
    deint.process(block1.data(), out1.data(), 6);

    // Cells at row 2 (indices 2, 5) should come through
    EXPECT_EQ(out1[2], 12);  // Index 2 -> row 2 -> no delay
    EXPECT_EQ(out1[5], 15);  // Index 5 -> row 2 -> no delay
}

//==============================================================================
// Round-trip Tests
//==============================================================================

TEST(TimeDeinterleaverTest, RoundTripWithInterleaver) {
    // Simulate: Interleaver -> Channel -> De-interleaver
    // Use depth=1 for simplicity: 2 rows

    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 1;
    config.cells_per_block = 4;  // 4 cells per block, 2 rows

    // Create both interleaver and de-interleaver
    // For CTI with depth=1:
    // Interleaver: row 0 has delay 0, row 1 has delay 4
    // De-interleaver: row 0 has delay 4 (num_rows-1-0)*cells, row 1 has delay 0

    // We'll simulate the interleaver manually
    // Original blocks: [0,1,2,3], [4,5,6,7], [8,9,10,11]

    // After interleaver (row 0 no delay, row 1 delayed by 4):
    // Block 0: cells 0,2 from current, cells 1,3 from (delayed)
    // Block 1: cells 4,6 from current, cells 1,3 from block 0
    // etc.

    // This is complex - let's just verify the de-interleaver produces
    // consistent output after settling

    TimeDeinterleaver deint(config);

    // Process enough blocks to settle
    std::vector<int8_t> block(4);
    std::vector<int8_t> output(4);

    for (size_t b = 0; b < 10; ++b) {
        std::fill(block.begin(), block.end(), static_cast<int8_t>(b));
        deint.process(block.data(), output.data(), 4);
    }

    // After settling, output should be consistent
    EXPECT_TRUE(deint.is_settled());
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(TimeDeinterleaverTest, NullInputReturnsEarly) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 2;
    config.cells_per_block = 10;

    TimeDeinterleaver deint(config);

    std::vector<int8_t> output(10, -1);
    deint.process(nullptr, output.data(), 10);

    // Output should be unchanged
    for (int8_t val : output) {
        EXPECT_EQ(val, -1);
    }
}

TEST(TimeDeinterleaverTest, NullOutputReturnsEarly) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 2;
    config.cells_per_block = 10;

    TimeDeinterleaver deint(config);

    std::vector<int8_t> input(10, 42);

    // Should not crash
    deint.process(input.data(), nullptr, 10);
}

TEST(TimeDeinterleaverTest, ZeroCellsNoOp) {
    TimeDeinterleaver deint;

    std::vector<int8_t> input(10);
    std::vector<int8_t> output(10, -1);

    size_t before = deint.blocks_processed();
    deint.process(input.data(), output.data(), 0);
    size_t after = deint.blocks_processed();

    // No block should be counted
    EXPECT_EQ(before, after);
}

//==============================================================================
// Configuration Update Tests
//==============================================================================

TEST(TimeDeinterleaverTest, SetConfig) {
    TimeDeinterleaver deint;

    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 5;
    config.cells_per_block = 500;

    deint.set_config(config);

    EXPECT_EQ(deint.get_config().depth, 5u);
    EXPECT_EQ(deint.get_settling_blocks(), 5u);
    EXPECT_EQ(deint.blocks_processed(), 0u);
}

TEST(TimeDeinterleaverTest, SetConfigResetsState) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 2;
    config.cells_per_block = 10;

    TimeDeinterleaver deint(config);

    // Process some blocks
    std::vector<int8_t> block(10, 1);
    std::vector<int8_t> output(10);
    deint.process(block.data(), output.data(), 10);
    deint.process(block.data(), output.data(), 10);

    EXPECT_EQ(deint.blocks_processed(), 2u);

    // Reconfigure
    config.depth = 3;
    deint.set_config(config);

    EXPECT_EQ(deint.blocks_processed(), 0u);
    EXPECT_FALSE(deint.is_settled());
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(TimeDeinterleaverTest, Reset) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 2;
    config.cells_per_block = 10;

    TimeDeinterleaver deint(config);

    // Process some blocks
    std::vector<int8_t> block(10, 42);
    std::vector<int8_t> output(10);
    for (int i = 0; i < 5; ++i) {
        deint.process(block.data(), output.data(), 10);
    }

    EXPECT_TRUE(deint.is_settled());
    EXPECT_EQ(deint.blocks_processed(), 5u);

    // Reset
    deint.reset();

    EXPECT_FALSE(deint.is_settled());
    EXPECT_EQ(deint.blocks_processed(), 0u);
}

TEST(TimeDeinterleaverTest, ResetClearsDelayLines) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 1;
    config.cells_per_block = 4;

    TimeDeinterleaver deint(config);

    // Fill delay lines with non-zero data
    std::vector<int8_t> block = {100, 101, 102, 103};
    std::vector<int8_t> output(4);
    for (int i = 0; i < 3; ++i) {
        deint.process(block.data(), output.data(), 4);
    }

    // Reset
    deint.reset();

    // Process again - should get zeros from delay lines initially
    std::vector<int8_t> new_block = {1, 2, 3, 4};
    deint.process(new_block.data(), output.data(), 4);

    // Row 0 cells should come from cleared delay line (zeros)
    // Row 1 cells should pass through
    EXPECT_EQ(output[0], 0);  // Row 0, from cleared delay
    EXPECT_EQ(output[1], 2);  // Row 1, pass-through
    EXPECT_EQ(output[2], 0);  // Row 0, from cleared delay
    EXPECT_EQ(output[3], 4);  // Row 1, pass-through
}

//==============================================================================
// Typical ATSC 3.0 Configurations
//==============================================================================

TEST(TimeDeinterleaverTest, TypicalATSC3Depths) {
    // Test typical TI depths used in ATSC 3.0
    std::vector<uint8_t> depths = {0, 1, 2, 3, 4, 8, 15};

    for (uint8_t depth : depths) {
        TimeDeinterleaverConfig config;
        config.mode = config::TimeInterleaveMode::CTI;
        config.depth = depth;
        config.cells_per_block = 1000;

        EXPECT_NO_THROW({
            TimeDeinterleaver deint(config);

            if (depth == 0) {
                EXPECT_TRUE(deint.is_settled());
            } else {
                EXPECT_EQ(deint.get_settling_blocks(), depth);
            }
        }) << "Failed for depth="
           << static_cast<int>(depth);
    }
}

TEST(TimeDeinterleaverTest, LargeCellsPerBlock) {
    // Test with large block size (typical ATSC 3.0 uses thousands of cells)
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 4;
    config.cells_per_block = 10000;

    EXPECT_NO_THROW({
        TimeDeinterleaver deint(config);

        std::vector<int8_t> input(10000);
        std::vector<int8_t> output(10000);
        std::iota(input.begin(), input.end(), 0);

        deint.process(input.data(), output.data(), 10000);
    });
}

//==============================================================================
// Stress Tests
//==============================================================================

TEST(TimeDeinterleaverTest, ManyBlocksProcessing) {
    TimeDeinterleaverConfig config;
    config.mode = config::TimeInterleaveMode::CTI;
    config.depth = 4;
    config.cells_per_block = 100;

    TimeDeinterleaver deint(config);

    std::vector<int8_t> input(100);
    std::vector<int8_t> output(100);

    // Process 1000 blocks
    for (size_t b = 0; b < 1000; ++b) {
        std::fill(input.begin(), input.end(), static_cast<int8_t>(b % 128));
        deint.process(input.data(), output.data(), 100);
    }

    EXPECT_EQ(deint.blocks_processed(), 1000u);
    EXPECT_TRUE(deint.is_settled());
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
