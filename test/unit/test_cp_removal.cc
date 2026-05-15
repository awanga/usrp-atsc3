// test_cp_removal.cc — Unit tests for lib/ofdm/cp_removal.h
//
// Tests cyclic prefix stripping for all defined CP fractions

#include "cp_removal.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

// Test compute_cp_length helper function
TEST(CpRemovalTest, ComputeCpLength) {
    // For 8K FFT
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k192_8192), 192u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k384_8192), 384u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k512_8192), 512u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k768_8192), 768u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k1024_8192), 1024u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k1536_8192), 1536u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k2048_8192), 2048u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k2432_8192), 2432u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k3072_8192), 3072u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k3648_8192), 3648u);
    EXPECT_EQ(compute_cp_length(8192, CpFraction::k4096_8192), 4096u);

    // For 16K FFT (CP lengths double)
    EXPECT_EQ(compute_cp_length(16384, CpFraction::k1024_8192), 2048u);
    EXPECT_EQ(compute_cp_length(16384, CpFraction::k2048_8192), 4096u);

    // For 32K FFT (CP lengths quadruple)
    EXPECT_EQ(compute_cp_length(32768, CpFraction::k1024_8192), 4096u);
    EXPECT_EQ(compute_cp_length(32768, CpFraction::k2048_8192), 8192u);
}

// Test basic construction
TEST(CpRemovalTest, Construction) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_config().fft_size, 8192u);
    EXPECT_EQ(cp_removal.get_cp_length(), 1024u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 8192u + 1024u);
    EXPECT_EQ(cp_removal.get_symbol_count(), 0u);
}

// Test invalid FFT size throws
TEST(CpRemovalTest, InvalidFftSizeThrows) {
    CpRemovalConfig config;
    config.fft_size = 4096;  // Bootstrap size, not valid for CP removal
    config.cp_fraction = CpFraction::k1024_8192;

    EXPECT_THROW(CpRemoval cp_removal(config), std::invalid_argument);
}

// Test valid FFT sizes
TEST(CpRemovalTest, ValidFftSizes) {
    CpRemovalConfig config;
    config.cp_fraction = CpFraction::k1024_8192;

    config.fft_size = 8192;
    EXPECT_NO_THROW(CpRemoval cp8k(config));

    config.fft_size = 16384;
    EXPECT_NO_THROW(CpRemoval cp16k(config));

    config.fft_size = 32768;
    EXPECT_NO_THROW(CpRemoval cp32k(config));
}

// Test CP removal with callback
TEST(CpRemovalTest, CallbackInvocation) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    std::vector<sample_t> received_symbol;
    size_t callback_count = 0;

    cp_removal.set_output_callback([&](const sample_t* symbol, size_t len) {
        received_symbol.assign(symbol, symbol + len);
        ++callback_count;
    });

    // Generate one complete symbol (CP + FFT data)
    size_t symbol_len = cp_removal.get_symbol_length();  // 1024 + 8192 = 9216
    std::vector<sample_t> input(symbol_len);

    // Fill CP with zeros, FFT data with incrementing pattern
    for (size_t i = 0; i < 1024; ++i) {
#ifdef ATSC3_FIXED_POINT
        input[i] = sample_t(0, 0);
#else
        input[i] = sample_t(0.0f, 0.0f);
#endif
    }
    for (size_t i = 0; i < 8192; ++i) {
#ifdef ATSC3_FIXED_POINT
        input[1024 + i] =
            sample_t(static_cast<int16_t>(i % 1000), static_cast<int16_t>((i + 500) % 1000));
#else
        input[1024 + i] = sample_t(static_cast<float>(i), static_cast<float>(i + 1));
#endif
    }

    // Process
    cp_removal.process(input.data(), input.size());

    // Should have received one symbol
    EXPECT_EQ(callback_count, 1u);
    EXPECT_EQ(cp_removal.get_symbol_count(), 1u);
    EXPECT_EQ(received_symbol.size(), 8192u);

    // Verify CP was stripped (output should start with FFT data, not CP zeros)
#ifdef ATSC3_FIXED_POINT
    EXPECT_EQ(received_symbol[0].real(), 0);
    EXPECT_EQ(received_symbol[1].real(), 1);
#else
    EXPECT_FLOAT_EQ(received_symbol[0].real(), 0.0f);
    EXPECT_FLOAT_EQ(received_symbol[1].real(), 1.0f);
#endif
}

// Test multiple symbols
TEST(CpRemovalTest, MultipleSymbols) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k512_8192;  // 512 sample CP

    CpRemoval cp_removal(config);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t len) {
        EXPECT_EQ(len, 8192u);
        ++callback_count;
    });

    // Send 3 complete symbols
    size_t symbol_len = cp_removal.get_symbol_length();  // 512 + 8192 = 8704
    std::vector<sample_t> input(symbol_len * 3, sample_t(0, 0));

    cp_removal.process(input.data(), input.size());

    EXPECT_EQ(callback_count, 3u);
    EXPECT_EQ(cp_removal.get_symbol_count(), 3u);
}

// Test partial symbol handling
TEST(CpRemovalTest, PartialSymbol) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t) { ++callback_count; });

    // Send partial symbol (less than CP + FFT)
    std::vector<sample_t> partial(5000, sample_t(0, 0));
    cp_removal.process(partial.data(), partial.size());

    EXPECT_EQ(callback_count, 0u);  // Not enough for a complete symbol

    // Send rest of symbol
    std::vector<sample_t> rest(9216 - 5000, sample_t(0, 0));
    cp_removal.process(rest.data(), rest.size());

    EXPECT_EQ(callback_count, 1u);  // Now we have one complete symbol
}

// Test sample-by-sample processing
TEST(CpRemovalTest, SampleBySample) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k192_8192;  // Smallest CP for faster test

    CpRemoval cp_removal(config);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t len) {
        EXPECT_EQ(len, 8192u);
        ++callback_count;
    });

    // Process one symbol sample by sample
    size_t symbol_len = cp_removal.get_symbol_length();  // 192 + 8192 = 8384
    for (size_t i = 0; i < symbol_len; ++i) {
        cp_removal.process(sample_t(0, 0));
    }

    EXPECT_EQ(callback_count, 1u);
}

// Test reset
TEST(CpRemovalTest, Reset) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t) { ++callback_count; });

    // Process partial data
    std::vector<sample_t> partial(5000, sample_t(0, 0));
    cp_removal.process(partial.data(), partial.size());

    EXPECT_EQ(callback_count, 0u);

    // Reset
    cp_removal.reset();
    EXPECT_EQ(cp_removal.get_symbol_count(), 0u);

    // Process full symbol
    std::vector<sample_t> full(9216, sample_t(0, 0));
    cp_removal.process(full.data(), full.size());

    EXPECT_EQ(callback_count, 1u);  // Reset cleared partial, fresh start
}

// Test reconfiguration
TEST(CpRemovalTest, Reconfigure) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_cp_length(), 1024u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 9216u);

    // Reconfigure to different CP
    CpRemovalConfig new_config;
    new_config.fft_size = 8192;
    new_config.cp_fraction = CpFraction::k2048_8192;

    cp_removal.reconfigure(new_config);

    EXPECT_EQ(cp_removal.get_cp_length(), 2048u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 10240u);
    EXPECT_EQ(cp_removal.get_symbol_count(), 0u);  // Reset on reconfigure
}

// Test reconfigure with invalid size throws
TEST(CpRemovalTest, ReconfigureInvalidThrows) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    CpRemovalConfig bad_config;
    bad_config.fft_size = 1024;  // Invalid
    bad_config.cp_fraction = CpFraction::k1024_8192;

    EXPECT_THROW(cp_removal.reconfigure(bad_config), std::invalid_argument);
}

// Test all CP fractions work with 8K FFT
TEST(CpRemovalTest, AllCpFractions8K) {
    std::vector<CpFraction> fractions = {
        CpFraction::k192_8192,  CpFraction::k384_8192,  CpFraction::k512_8192,
        CpFraction::k768_8192,  CpFraction::k1024_8192, CpFraction::k1536_8192,
        CpFraction::k2048_8192, CpFraction::k2432_8192, CpFraction::k3072_8192,
        CpFraction::k3648_8192, CpFraction::k4096_8192};

    for (CpFraction frac : fractions) {
        CpRemovalConfig config;
        config.fft_size = 8192;
        config.cp_fraction = frac;

        CpRemoval cp_removal(config);

        size_t expected_cp = compute_cp_length(8192, frac);
        EXPECT_EQ(cp_removal.get_cp_length(), expected_cp)
            << "Mismatch for CP fraction " << static_cast<size_t>(frac);

        // Process one symbol
        size_t callback_count = 0;
        cp_removal.set_output_callback([&](const sample_t*, size_t len) {
            EXPECT_EQ(len, 8192u);
            ++callback_count;
        });

        std::vector<sample_t> input(cp_removal.get_symbol_length(), sample_t(0, 0));
        cp_removal.process(input.data(), input.size());

        EXPECT_EQ(callback_count, 1u)
            << "No callback for CP fraction " << static_cast<size_t>(frac);
    }
}

// Test 16K FFT
TEST(CpRemovalTest, Fft16K) {
    CpRemovalConfig config;
    config.fft_size = 16384;
    config.cp_fraction = CpFraction::k1024_8192;  // = 2048 for 16K

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_cp_length(), 2048u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 18432u);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t len) {
        EXPECT_EQ(len, 16384u);
        ++callback_count;
    });

    std::vector<sample_t> input(18432, sample_t(0, 0));
    cp_removal.process(input.data(), input.size());

    EXPECT_EQ(callback_count, 1u);
}

// Test 32K FFT
TEST(CpRemovalTest, Fft32K) {
    CpRemovalConfig config;
    config.fft_size = 32768;
    config.cp_fraction = CpFraction::k512_8192;  // = 2048 for 32K

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_cp_length(), 2048u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 34816u);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t len) {
        EXPECT_EQ(len, 32768u);
        ++callback_count;
    });

    std::vector<sample_t> input(34816, sample_t(0, 0));
    cp_removal.process(input.data(), input.size());

    EXPECT_EQ(callback_count, 1u);
}

// Test no callback set (should not crash)
TEST(CpRemovalTest, NoCallbackSet) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);
    // No callback set

    std::vector<sample_t> input(9216, sample_t(0, 0));

    // Should not crash
    EXPECT_NO_THROW(cp_removal.process(input.data(), input.size()));
    EXPECT_EQ(cp_removal.get_symbol_count(), 1u);
}

// Test data integrity through CP removal
TEST(CpRemovalTest, DataIntegrity) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.cp_fraction = CpFraction::k1024_8192;

    CpRemoval cp_removal(config);

    std::vector<sample_t> received;
    cp_removal.set_output_callback(
        [&](const sample_t* symbol, size_t len) { received.assign(symbol, symbol + len); });

    // Create symbol with known pattern
    std::vector<sample_t> input(9216);

    // CP: fill with marker value
    for (size_t i = 0; i < 1024; ++i) {
#ifdef ATSC3_FIXED_POINT
        input[i] = sample_t(-1000, -1000);
#else
        input[i] = sample_t(-999.0f, -999.0f);
#endif
    }

    // FFT data: fill with index values
    for (size_t i = 0; i < 8192; ++i) {
#ifdef ATSC3_FIXED_POINT
        input[1024 + i] =
            sample_t(static_cast<int16_t>(i % 32767), static_cast<int16_t>((i * 2) % 32767));
#else
        input[1024 + i] = sample_t(static_cast<float>(i), static_cast<float>(i * 2));
#endif
    }

    cp_removal.process(input.data(), input.size());

    ASSERT_EQ(received.size(), 8192u);

    // Verify received data matches FFT portion (CP stripped)
    for (size_t i = 0; i < 8192; ++i) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_EQ(received[i].real(), static_cast<int16_t>(i % 32767)) << "at index " << i;
        EXPECT_EQ(received[i].imag(), static_cast<int16_t>((i * 2) % 32767)) << "at index " << i;
#else
        EXPECT_FLOAT_EQ(received[i].real(), static_cast<float>(i)) << "at index " << i;
        EXPECT_FLOAT_EQ(received[i].imag(), static_cast<float>(i * 2)) << "at index " << i;
#endif
    }
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
