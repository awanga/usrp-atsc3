// test_cell_deinterleaver.cc — Unit tests for lib/ofdm/cell_deinterleaver.h
//
// Tests cell de-interleaving and round-trip verification

#include "cell_deinterleaver.h"

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

TEST(CellDeinterleaverTest, DefaultConstruction) {
    CellDeinterleaver deint;

    auto config = deint.get_config();
    EXPECT_EQ(config.num_cells, 0u);
    EXPECT_TRUE(deint.get_permutation().empty());
}

TEST(CellDeinterleaverTest, ConstructWithConfig) {
    CellDeinterleaverConfig config;
    config.num_cells = 100;

    CellDeinterleaver deint(config);

    EXPECT_EQ(deint.get_config().num_cells, 100u);
    EXPECT_EQ(deint.get_permutation().size(), 100u);
}

TEST(CellDeinterleaverTest, ConstructWithPowerOfTwoCells) {
    CellDeinterleaverConfig config;
    config.num_cells = 128;

    CellDeinterleaver deint(config);

    EXPECT_EQ(deint.get_permutation().size(), 128u);
}

//==============================================================================
// Permutation Tests
//==============================================================================

TEST(CellDeinterleaverTest, PermutationIsValidPermutation) {
    CellDeinterleaverConfig config;
    config.num_cells = 100;

    CellDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    // Check that permutation contains each index exactly once
    std::vector<bool> seen(100, false);
    for (size_t i = 0; i < 100; ++i) {
        ASSERT_LT(perm[i], 100u) << "Permutation value out of range at index " << i;
        EXPECT_FALSE(seen[perm[i]]) << "Duplicate value " << perm[i] << " in permutation";
        seen[perm[i]] = true;
    }
}

TEST(CellDeinterleaverTest, PermutationIsNotIdentity) {
    CellDeinterleaverConfig config;
    config.num_cells = 64;  // Power of 2 for clear bit-reversal pattern

    CellDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    // At least some elements should be permuted (not identity)
    bool has_non_identity = false;
    for (size_t i = 0; i < 64; ++i) {
        if (perm[i] != i) {
            has_non_identity = true;
            break;
        }
    }
    EXPECT_TRUE(has_non_identity) << "Permutation should not be identity";
}

TEST(CellDeinterleaverTest, PermutationForSingleCell) {
    CellDeinterleaverConfig config;
    config.num_cells = 1;

    CellDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    ASSERT_EQ(perm.size(), 1u);
    EXPECT_EQ(perm[0], 0u);
}

//==============================================================================
// De-interleaving Tests (Out-of-place)
//==============================================================================

TEST(CellDeinterleaverTest, DeinterleaveOutOfPlace) {
    CellDeinterleaverConfig config;
    config.num_cells = 16;

    CellDeinterleaver deint(config);

    std::vector<int8_t> input(16);
    std::iota(input.begin(), input.end(), 0);  // 0, 1, 2, ..., 15

    std::vector<int8_t> output(16);
    deint.deinterleave(input.data(), output.data(), 16);

    // Verify output is a permutation of input
    std::vector<int8_t> sorted_output = output;
    std::sort(sorted_output.begin(), sorted_output.end());
    EXPECT_EQ(sorted_output, input);
}

TEST(CellDeinterleaverTest, DeinterleaveNullInputReturnsEarly) {
    CellDeinterleaverConfig config;
    config.num_cells = 16;
    CellDeinterleaver deint(config);

    std::vector<int8_t> output(16, -1);
    deint.deinterleave(nullptr, output.data(), 16);

    // Output should be unchanged
    for (int8_t val : output) {
        EXPECT_EQ(val, -1);
    }
}

TEST(CellDeinterleaverTest, DeinterleaveNullOutputReturnsEarly) {
    CellDeinterleaverConfig config;
    config.num_cells = 16;
    CellDeinterleaver deint(config);

    std::vector<int8_t> input(16);
    std::iota(input.begin(), input.end(), 0);

    // Should not crash
    deint.deinterleave(input.data(), nullptr, 16);
}

TEST(CellDeinterleaverTest, DeinterleaveZeroCells) {
    CellDeinterleaver deint;

    std::vector<int8_t> input(16);
    std::vector<int8_t> output(16, -1);

    deint.deinterleave(input.data(), output.data(), 0);

    // Output should be unchanged
    for (int8_t val : output) {
        EXPECT_EQ(val, -1);
    }
}

//==============================================================================
// De-interleaving Tests (In-place)
//==============================================================================

TEST(CellDeinterleaverTest, DeinterleaveInPlace) {
    CellDeinterleaverConfig config;
    config.num_cells = 32;

    CellDeinterleaver deint(config);

    std::vector<int8_t> data(32);
    std::iota(data.begin(), data.end(), 0);

    std::vector<int8_t> original = data;

    deint.deinterleave(data.data(), 32);

    // Verify result is a permutation
    std::vector<int8_t> sorted_data = data;
    std::sort(sorted_data.begin(), sorted_data.end());
    EXPECT_EQ(sorted_data, original);
}

TEST(CellDeinterleaverTest, DeinterleaveInPlaceNullInput) {
    CellDeinterleaver deint;

    // Should not crash
    deint.deinterleave(nullptr, 10);
}

//==============================================================================
// Round-trip Tests (Interleave + De-interleave)
//==============================================================================

TEST(CellDeinterleaverTest, RoundTripRecovery) {
    // Key requirement: interleave then de-interleave recovers original

    CellDeinterleaverConfig config;
    config.num_cells = 64;
    CellDeinterleaver deint(config);

    // Original data
    std::vector<int8_t> original(64);
    std::iota(original.begin(), original.end(), -32);  // -32, -31, ..., 31

    // Simulate interleaving (apply forward permutation)
    // The de-interleaver has inv_permutation where inv_perm[i] = source for output i
    // The interleaver does: output[perm[i]] = input[i]
    // So we need to build the forward permutation

    const auto& inv_perm = deint.get_permutation();
    std::vector<size_t> fwd_perm(64);
    for (size_t i = 0; i < 64; ++i) {
        fwd_perm[inv_perm[i]] = i;
    }

    // Interleave
    std::vector<int8_t> interleaved(64);
    for (size_t i = 0; i < 64; ++i) {
        interleaved[fwd_perm[i]] = original[i];
    }

    // De-interleave
    std::vector<int8_t> recovered(64);
    deint.deinterleave(interleaved.data(), recovered.data(), 64);

    // Verify recovery
    EXPECT_EQ(recovered, original);
}

TEST(CellDeinterleaverTest, RoundTripWithRandomData) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(-128, 127);

    for (size_t num_cells : {16, 100, 256, 1000}) {
        CellDeinterleaverConfig config;
        config.num_cells = num_cells;
        CellDeinterleaver deint(config);

        // Random original data
        std::vector<int8_t> original(num_cells);
        for (size_t i = 0; i < num_cells; ++i) {
            original[i] = static_cast<int8_t>(dist(rng));
        }

        // Build forward permutation from inverse
        const auto& inv_perm = deint.get_permutation();
        std::vector<size_t> fwd_perm(num_cells);
        for (size_t i = 0; i < num_cells; ++i) {
            fwd_perm[inv_perm[i]] = i;
        }

        // Interleave
        std::vector<int8_t> interleaved(num_cells);
        for (size_t i = 0; i < num_cells; ++i) {
            interleaved[fwd_perm[i]] = original[i];
        }

        // De-interleave
        std::vector<int8_t> recovered(num_cells);
        deint.deinterleave(interleaved.data(), recovered.data(), num_cells);

        EXPECT_EQ(recovered, original) << "Round-trip failed for num_cells=" << num_cells;
    }
}

//==============================================================================
// Configuration Update Tests
//==============================================================================

TEST(CellDeinterleaverTest, SetConfig) {
    CellDeinterleaver deint;

    EXPECT_EQ(deint.get_permutation().size(), 0u);

    CellDeinterleaverConfig config;
    config.num_cells = 50;
    deint.set_config(config);

    EXPECT_EQ(deint.get_permutation().size(), 50u);
}

TEST(CellDeinterleaverTest, SetConfigDifferentSize) {
    CellDeinterleaverConfig config1;
    config1.num_cells = 100;
    CellDeinterleaver deint(config1);

    EXPECT_EQ(deint.get_permutation().size(), 100u);

    CellDeinterleaverConfig config2;
    config2.num_cells = 200;
    deint.set_config(config2);

    EXPECT_EQ(deint.get_permutation().size(), 200u);
}

//==============================================================================
// Auto-resize Tests
//==============================================================================

TEST(CellDeinterleaverTest, InPlaceAutoResize) {
    CellDeinterleaverConfig config;
    config.num_cells = 32;
    CellDeinterleaver deint(config);

    // Pass different size - should auto-recompute permutation
    std::vector<int8_t> data(64);
    std::iota(data.begin(), data.end(), 0);

    deint.deinterleave(data.data(), 64);

    // Verify permutation was resized
    EXPECT_EQ(deint.get_permutation().size(), 64u);
}

//==============================================================================
// Typical ATSC 3.0 Cell Counts
//==============================================================================

TEST(CellDeinterleaverTest, TypicalATSC3CellCounts) {
    // Test with typical ATSC 3.0 FEC block cell counts
    // Long LDPC (64800 bits) / bits per symbol

    std::vector<size_t> typical_counts = {
        64800 / 2,   // QPSK: 32400 cells
        64800 / 4,   // 16-QAM: 16200 cells
        64800 / 6,   // 64-QAM: 10800 cells
        64800 / 8,   // 256-QAM: 8100 cells
        64800 / 10,  // 1024-QAM: 6480 cells
        64800 / 12,  // 4096-QAM: 5400 cells
        16200 / 6,   // Short LDPC 64-QAM: 2700 cells
    };

    for (size_t num_cells : typical_counts) {
        CellDeinterleaverConfig config;
        config.num_cells = num_cells;

        EXPECT_NO_THROW({
            CellDeinterleaver deint(config);
            EXPECT_EQ(deint.get_permutation().size(), num_cells);
        }) << "Failed for num_cells="
           << num_cells;
    }
}

//==============================================================================
// Utility Function Tests
//==============================================================================

TEST(CellDeinterleaverTest, ComputeCellInterleaverPosition) {
    // Test that utility function returns valid positions
    size_t num_cells = 100;

    std::vector<bool> seen(num_cells, false);
    for (size_t i = 0; i < num_cells; ++i) {
        size_t pos = compute_cell_interleaver_position(i, num_cells);
        ASSERT_LT(pos, num_cells) << "Position out of range for input " << i;
        EXPECT_FALSE(seen[pos]) << "Duplicate position " << pos;
        seen[pos] = true;
    }
}

TEST(CellDeinterleaverTest, ComputeCellInterleaverPositionOutOfRange) {
    // Out-of-range input should return input unchanged
    EXPECT_EQ(compute_cell_interleaver_position(100, 100), 100u);
    EXPECT_EQ(compute_cell_interleaver_position(50, 0), 50u);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(CellDeinterleaverTest, Reset) {
    CellDeinterleaverConfig config;
    config.num_cells = 50;
    CellDeinterleaver deint(config);

    // Reset should not change permutation
    auto perm_before = deint.get_permutation();
    deint.reset();
    auto perm_after = deint.get_permutation();

    EXPECT_EQ(perm_before, perm_after);
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
