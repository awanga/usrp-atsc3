// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_cp_removal.cc — Unit tests for lib/ofdm/cp_removal.h
//
// Guard interval lengths and the FFT sizes each may be used with are from
// A/322 Table 8.9; the pattern numbering is the Table 9.13 L1 code.

#include "cp_removal.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

// A/322 Table 8.9 lengths, the same for every FFT size; Table 9.13 codes
// 0 and 13-15 are reserved.
TEST(CpRemovalTest, GuardIntervalSamplesFollowTable89) {
    const size_t samples[13] = {0,    192,  384,  512,  768,  1024, 1536,
                                2048, 2432, 3072, 3648, 4096, 4864};
    for (unsigned code = 0; code <= 15; ++code) {
        size_t expected = code <= 12 ? samples[code] : 0;
        EXPECT_EQ(guard_interval_samples(static_cast<GuardInterval>(code)), expected)
            << "code " << code;
    }
}

TEST(CpRemovalTest, GuardIntervalFromSamples) {
    const size_t samples[13] = {0,    192,  384,  512,  768,  1024, 1536,
                                2048, 2432, 3072, 3648, 4096, 4864};
    for (unsigned code = 1; code <= 12; ++code) {
        GuardInterval gi = GuardInterval::GI1_192;
        ASSERT_TRUE(guard_interval_from_samples(samples[code], &gi)) << samples[code];
        EXPECT_EQ(static_cast<unsigned>(gi), code);
    }
    GuardInterval gi = GuardInterval::GI1_192;
    for (size_t bad : {size_t{0}, size_t{191}, size_t{193}, size_t{8192}}) {
        EXPECT_FALSE(guard_interval_from_samples(bad, &gi)) << bad;
    }
}

// A/322 Table 8.9: GI1-GI7 for every FFT size, GI8-GI11 not with 8K,
// GI12 only with 32K.
TEST(CpRemovalTest, GuardIntervalAllowedFollowsTable89) {
    for (unsigned code = 0; code <= 15; ++code) {
        GuardInterval gi = static_cast<GuardInterval>(code);
        bool defined = code >= 1 && code <= 12;
        EXPECT_EQ(guard_interval_allowed(gi, 8192), defined && code <= 7) << code;
        EXPECT_EQ(guard_interval_allowed(gi, 16384), defined && code <= 11) << code;
        EXPECT_EQ(guard_interval_allowed(gi, 32768), defined) << code;
        EXPECT_FALSE(guard_interval_allowed(gi, 2048)) << code;
    }
}

// Test basic construction
TEST(CpRemovalTest, Construction) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI5_1024;

    EXPECT_THROW(CpRemoval cp_removal(config), std::invalid_argument);
}

// Test valid FFT sizes
TEST(CpRemovalTest, ValidFftSizes) {
    CpRemovalConfig config;
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI3_512;

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
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI1_192;

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
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI5_1024;

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_cp_length(), 1024u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 9216u);

    // Reconfigure to different CP
    CpRemovalConfig new_config;
    new_config.fft_size = 8192;
    new_config.guard_interval = GuardInterval::GI7_2048;

    cp_removal.reconfigure(new_config);

    EXPECT_EQ(cp_removal.get_cp_length(), 2048u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 10240u);
    EXPECT_EQ(cp_removal.get_symbol_count(), 0u);  // Reset on reconfigure
}

// Test reconfigure with invalid size throws
TEST(CpRemovalTest, ReconfigureInvalidThrows) {
    CpRemovalConfig config;
    config.fft_size = 8192;
    config.guard_interval = GuardInterval::GI5_1024;

    CpRemoval cp_removal(config);

    CpRemovalConfig bad_config;
    bad_config.fft_size = 1024;  // Invalid
    bad_config.guard_interval = GuardInterval::GI5_1024;

    EXPECT_THROW(cp_removal.reconfigure(bad_config), std::invalid_argument);
}

// Every allowed FFT size / guard interval pair strips exactly the Table
// 8.9 length; the others are rejected.
TEST(CpRemovalTest, EveryTable89Combination) {
    const size_t samples[13] = {0,    192,  384,  512,  768,  1024, 1536,
                                2048, 2432, 3072, 3648, 4096, 4864};
    for (size_t fft : {size_t{8192}, size_t{16384}, size_t{32768}}) {
        for (unsigned code = 1; code <= 12; ++code) {
            CpRemovalConfig config;
            config.fft_size = fft;
            config.guard_interval = static_cast<GuardInterval>(code);
            bool allowed = code <= 7 || (code <= 11 && fft >= 16384) || fft == 32768;
            if (!allowed) {
                EXPECT_THROW(CpRemoval c(config), std::invalid_argument) << fft << " GI" << code;
                continue;
            }
            CpRemoval cp_removal(config);
            EXPECT_EQ(cp_removal.get_cp_length(), samples[code]) << fft << " GI" << code;

            // Mark the last guard sample and the first useful one.
            std::vector<sample_t> input(samples[code] + fft, sample_t(0, 0));
            input[samples[code] - 1] = sample_t(7, 7);
            input[samples[code]] = sample_t(1, 1);
            size_t callbacks = 0;
            cp_removal.set_output_callback([&](const sample_t* sym, size_t len) {
                EXPECT_EQ(len, fft);
                EXPECT_EQ(sym[0], sample_t(1, 1)) << fft << " GI" << code;
                ++callbacks;
            });
            cp_removal.process(input.data(), input.size());
            EXPECT_EQ(callbacks, 1u) << fft << " GI" << code;
        }
    }
}

TEST(CpRemovalTest, ReconfigureRejectsDisallowedGuardInterval) {
    CpRemovalConfig config;
    CpRemoval cp_removal(config);
    CpRemovalConfig bad;
    bad.fft_size = 8192;
    bad.guard_interval = GuardInterval::GI8_2432;  // N/A for 8K
    EXPECT_THROW(cp_removal.reconfigure(bad), std::invalid_argument);
    EXPECT_EQ(cp_removal.get_cp_length(), 1024u);
}

// Test 16K FFT
TEST(CpRemovalTest, Fft16K) {
    CpRemovalConfig config;
    config.fft_size = 16384;
    config.guard_interval = GuardInterval::GI5_1024;

    CpRemoval cp_removal(config);

    EXPECT_EQ(cp_removal.get_cp_length(), 1024u);
    EXPECT_EQ(cp_removal.get_symbol_length(), 17408u);

    size_t callback_count = 0;
    cp_removal.set_output_callback([&](const sample_t*, size_t len) {
        EXPECT_EQ(len, 16384u);
        ++callback_count;
    });

    std::vector<sample_t> input(17408, sample_t(0, 0));
    cp_removal.process(input.data(), input.size());

    EXPECT_EQ(callback_count, 1u);
}

// Test 32K FFT
TEST(CpRemovalTest, Fft32K) {
    CpRemovalConfig config;
    config.fft_size = 32768;
    config.guard_interval = GuardInterval::GI7_2048;

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
    config.guard_interval = GuardInterval::GI5_1024;

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
    config.guard_interval = GuardInterval::GI5_1024;

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
