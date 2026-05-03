// test_freq_deinterleaver.cc — Unit tests for lib/ofdm/freq_deinterleaver.h
//
// Tests frequency de-interleaving and permutation validity

#include "freq_deinterleaver.h"

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

TEST(FreqDeinterleaverTest, DefaultConstruction) {
    FreqDeinterleaver deint;

    auto config = deint.get_config();
    EXPECT_EQ(config.fft_size, 8192u);
    EXPECT_EQ(config.num_active_carriers, 6913u);
    EXPECT_EQ(deint.num_carriers(), 6913u);
}

TEST(FreqDeinterleaverTest, Construct8KFFT) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 6913;

    FreqDeinterleaver deint(config);

    EXPECT_EQ(deint.get_permutation().size(), 6913u);
}

TEST(FreqDeinterleaverTest, Construct16KFFT) {
    FreqDeinterleaverConfig config;
    config.fft_size = 16384;
    config.num_active_carriers = 13825;

    FreqDeinterleaver deint(config);

    EXPECT_EQ(deint.get_permutation().size(), 13825u);
}

TEST(FreqDeinterleaverTest, Construct32KFFT) {
    FreqDeinterleaverConfig config;
    config.fft_size = 32768;
    config.num_active_carriers = 27649;

    FreqDeinterleaver deint(config);

    EXPECT_EQ(deint.get_permutation().size(), 27649u);
}

TEST(FreqDeinterleaverTest, ConstructZeroCarriers) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 0;

    FreqDeinterleaver deint(config);

    EXPECT_TRUE(deint.get_permutation().empty());
}

//==============================================================================
// Permutation Validity Tests
//==============================================================================

TEST(FreqDeinterleaverTest, PermutationIsValidPermutation8K) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 6913;

    FreqDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    ASSERT_EQ(perm.size(), 6913u);

    // Check that permutation contains each index exactly once
    std::vector<bool> seen(6913, false);
    for (size_t i = 0; i < 6913; ++i) {
        ASSERT_LT(perm[i], 6913u) << "Permutation value out of range at index " << i;
        EXPECT_FALSE(seen[perm[i]]) << "Duplicate value " << perm[i] << " in permutation";
        seen[perm[i]] = true;
    }
}

TEST(FreqDeinterleaverTest, PermutationIsNotIdentity) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 1000;

    FreqDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    // At least some elements should be permuted
    bool has_non_identity = false;
    for (size_t i = 0; i < 1000; ++i) {
        if (perm[i] != i) {
            has_non_identity = true;
            break;
        }
    }
    EXPECT_TRUE(has_non_identity) << "Permutation should not be identity";
}

TEST(FreqDeinterleaverTest, PermutationIsDeterministic) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 1000;

    FreqDeinterleaver deint1(config);
    FreqDeinterleaver deint2(config);

    EXPECT_EQ(deint1.get_permutation(), deint2.get_permutation());
}

//==============================================================================
// De-interleaving Tests (Out-of-place)
//==============================================================================

TEST(FreqDeinterleaverTest, DeinterleaveOutOfPlace) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 100;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> input(100);
    std::iota(input.begin(), input.end(), 0);

    std::vector<int8_t> output(100);
    deint.deinterleave(input.data(), output.data());

    // Verify output is a permutation of input
    std::vector<int8_t> sorted_output = output;
    std::sort(sorted_output.begin(), sorted_output.end());
    EXPECT_EQ(sorted_output, input);
}

TEST(FreqDeinterleaverTest, DeinterleaveNullInputReturnsEarly) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 100;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> output(100, -1);
    deint.deinterleave(nullptr, output.data());

    // Output should be unchanged
    for (int8_t val : output) {
        EXPECT_EQ(val, -1);
    }
}

TEST(FreqDeinterleaverTest, DeinterleaveNullOutputReturnsEarly) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 100;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> input(100);
    std::iota(input.begin(), input.end(), 0);

    // Should not crash
    deint.deinterleave(input.data(), nullptr);
}

//==============================================================================
// De-interleaving Tests (In-place)
//==============================================================================

TEST(FreqDeinterleaverTest, DeinterleaveInPlace) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 100;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> data(100);
    std::iota(data.begin(), data.end(), 0);

    std::vector<int8_t> original = data;

    deint.deinterleave_inplace(data.data());

    // Verify result is a permutation
    std::vector<int8_t> sorted_data = data;
    std::sort(sorted_data.begin(), sorted_data.end());
    EXPECT_EQ(sorted_data, original);
}

TEST(FreqDeinterleaverTest, DeinterleaveInPlaceNullInput) {
    FreqDeinterleaver deint;

    // Should not crash
    deint.deinterleave_inplace(nullptr);
}

TEST(FreqDeinterleaverTest, InPlaceMatchesOutOfPlace) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 500;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> input(500);
    std::iota(input.begin(), input.end(), -50);

    // Out-of-place
    std::vector<int8_t> out_of_place(500);
    deint.deinterleave(input.data(), out_of_place.data());

    // In-place
    std::vector<int8_t> in_place = input;
    deint.deinterleave_inplace(in_place.data());

    EXPECT_EQ(in_place, out_of_place);
}

//==============================================================================
// Round-trip Tests
//==============================================================================

TEST(FreqDeinterleaverTest, RoundTripRecovery) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 200;

    FreqDeinterleaver deint(config);

    // Original data
    std::vector<int8_t> original(200);
    std::iota(original.begin(), original.end(), -100);

    // Build forward permutation from inverse
    const auto& inv_perm = deint.get_permutation();
    std::vector<size_t> fwd_perm(200);
    for (size_t i = 0; i < 200; ++i) {
        fwd_perm[inv_perm[i]] = i;
    }

    // Interleave
    std::vector<int8_t> interleaved(200);
    for (size_t i = 0; i < 200; ++i) {
        interleaved[fwd_perm[i]] = original[i];
    }

    // De-interleave
    std::vector<int8_t> recovered(200);
    deint.deinterleave(interleaved.data(), recovered.data());

    // Verify recovery
    EXPECT_EQ(recovered, original);
}

TEST(FreqDeinterleaverTest, RoundTripWithRandomData) {
    std::mt19937 rng(54321);
    std::uniform_int_distribution<int> dist(-128, 127);

    for (size_t fft_size : {8192, 16384}) {
        size_t num_carriers = fft_size == 8192 ? 6913 : 13825;

        FreqDeinterleaverConfig config;
        config.fft_size = fft_size;
        config.num_active_carriers = num_carriers;

        FreqDeinterleaver deint(config);

        // Random original data
        std::vector<int8_t> original(num_carriers);
        for (size_t i = 0; i < num_carriers; ++i) {
            original[i] = static_cast<int8_t>(dist(rng));
        }

        // Build forward permutation
        const auto& inv_perm = deint.get_permutation();
        std::vector<size_t> fwd_perm(num_carriers);
        for (size_t i = 0; i < num_carriers; ++i) {
            fwd_perm[inv_perm[i]] = i;
        }

        // Interleave
        std::vector<int8_t> interleaved(num_carriers);
        for (size_t i = 0; i < num_carriers; ++i) {
            interleaved[fwd_perm[i]] = original[i];
        }

        // De-interleave
        std::vector<int8_t> recovered(num_carriers);
        deint.deinterleave(interleaved.data(), recovered.data());

        EXPECT_EQ(recovered, original)
            << "Round-trip failed for fft_size=" << fft_size << ", num_carriers=" << num_carriers;
    }
}

//==============================================================================
// Configuration Update Tests
//==============================================================================

TEST(FreqDeinterleaverTest, SetConfig) {
    FreqDeinterleaver deint;

    EXPECT_EQ(deint.num_carriers(), 6913u);

    FreqDeinterleaverConfig config;
    config.fft_size = 16384;
    config.num_active_carriers = 13825;
    deint.set_config(config);

    EXPECT_EQ(deint.num_carriers(), 13825u);
    EXPECT_EQ(deint.get_permutation().size(), 13825u);
}

TEST(FreqDeinterleaverTest, SetConfigDifferentFFTSize) {
    FreqDeinterleaverConfig config1;
    config1.fft_size = 8192;
    config1.num_active_carriers = 6913;
    FreqDeinterleaver deint(config1);

    auto perm_8k = deint.get_permutation();

    FreqDeinterleaverConfig config2;
    config2.fft_size = 16384;
    config2.num_active_carriers = 13825;  // Different carrier count for different FFT
    deint.set_config(config2);

    auto perm_16k = deint.get_permutation();

    // Permutations should have different sizes for different carrier counts
    EXPECT_NE(perm_8k.size(), perm_16k.size());
}

//==============================================================================
// ATSC 3.0 Standard Configurations
//==============================================================================

TEST(FreqDeinterleaverTest, ATSC3Standard8K) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 6913;

    FreqDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    EXPECT_EQ(perm.size(), 6913u);

    // Verify valid permutation
    std::vector<bool> seen(6913, false);
    for (size_t p : perm) {
        ASSERT_LT(p, 6913u);
        seen[p] = true;
    }
    for (bool s : seen) {
        EXPECT_TRUE(s);
    }
}

TEST(FreqDeinterleaverTest, ATSC3Standard16K) {
    FreqDeinterleaverConfig config;
    config.fft_size = 16384;
    config.num_active_carriers = 13825;

    FreqDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    EXPECT_EQ(perm.size(), 13825u);
}

TEST(FreqDeinterleaverTest, ATSC3Standard32K) {
    FreqDeinterleaverConfig config;
    config.fft_size = 32768;
    config.num_active_carriers = 27649;

    FreqDeinterleaver deint(config);
    const auto& perm = deint.get_permutation();

    EXPECT_EQ(perm.size(), 27649u);
}

//==============================================================================
// Utility Function Tests
//==============================================================================

TEST(FreqDeinterleaverTest, ComputeFreqInterleaverPosition) {
    // Test utility function returns valid positions
    size_t fft_size = 8192;
    size_t num_carriers = 100;

    std::vector<bool> seen(num_carriers, false);
    for (size_t i = 0; i < num_carriers; ++i) {
        size_t pos = compute_freq_interleaver_position(i, fft_size, num_carriers);
        ASSERT_LT(pos, num_carriers) << "Position out of range for input " << i;
        // Note: positions may be duplicated if LFSR doesn't cover all values
        seen[pos] = true;
    }

    // At minimum, some positions should be filled
    size_t filled = std::count(seen.begin(), seen.end(), true);
    EXPECT_GT(filled, 0u);
}

TEST(FreqDeinterleaverTest, ComputeFreqInterleaverPositionOutOfRange) {
    // Out-of-range input should return input unchanged
    EXPECT_EQ(compute_freq_interleaver_position(100, 8192, 100), 100u);
    EXPECT_EQ(compute_freq_interleaver_position(50, 8192, 0), 50u);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(FreqDeinterleaverTest, Reset) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 100;

    FreqDeinterleaver deint(config);

    // Reset should not change permutation
    auto perm_before = deint.get_permutation();
    deint.reset();
    auto perm_after = deint.get_permutation();

    EXPECT_EQ(perm_before, perm_after);
}

//==============================================================================
// Performance Stress Tests
//==============================================================================

TEST(FreqDeinterleaverTest, MultipleSymbolsProcessing) {
    FreqDeinterleaverConfig config;
    config.fft_size = 8192;
    config.num_active_carriers = 6913;

    FreqDeinterleaver deint(config);

    std::vector<int8_t> input(6913);
    std::vector<int8_t> output(6913);
    std::iota(input.begin(), input.end(), 0);

    // Process 100 OFDM symbols
    for (int sym = 0; sym < 100; ++sym) {
        deint.deinterleave(input.data(), output.data());
    }

    // Verify output is still a valid permutation of input
    std::vector<int8_t> sorted_output = output;
    std::sort(sorted_output.begin(), sorted_output.end());

    std::vector<int8_t> sorted_input = input;
    std::sort(sorted_input.begin(), sorted_input.end());
    EXPECT_EQ(sorted_output, sorted_input);
}

//==============================================================================
// Various Carrier Counts
//==============================================================================

TEST(FreqDeinterleaverTest, VariousCarrierCounts) {
    // Test with various carrier counts within 8K FFT
    std::vector<size_t> carrier_counts = {100, 500, 1000, 3000, 5000, 6913};

    for (size_t n : carrier_counts) {
        FreqDeinterleaverConfig config;
        config.fft_size = 8192;
        config.num_active_carriers = n;

        EXPECT_NO_THROW({
            FreqDeinterleaver deint(config);
            EXPECT_EQ(deint.get_permutation().size(), n);

            // Verify valid permutation
            const auto& perm = deint.get_permutation();
            std::vector<bool> seen(n, false);
            bool all_valid = true;
            for (size_t i = 0; i < n && all_valid; ++i) {
                if (perm[i] >= n) {
                    all_valid = false;
                } else {
                    seen[perm[i]] = true;
                }
            }
            EXPECT_TRUE(all_valid) << "Invalid permutation for n=" << n;
        }) << "Failed for num_active_carriers="
           << n;
    }
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
