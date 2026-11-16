// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_bootstrap_signal.cc — Unit tests for lib/sync/bootstrap_signal.h
//
// Expected values: the frequency-domain sequence from the test-side model
// in bootstrap_tx_model.h (A/321 5.2), the Gray-code example of A/321
// Table B.1.1, the minimum-time values of Table 6.3, the bsr_coefficient
// values of A/322 Table 9.1, and every row of A/322 Table H.1.1 as printed.

#include "bootstrap_tx_model.h"
#include "sync/bootstrap_signal.h"

#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace sync {
namespace {

using test::BootstrapTxModel;

TEST(BootstrapSignalTest, VersionParametersFollowTables61And66) {
    unsigned root = 0;
    uint16_t seed = 0;
    ASSERT_TRUE(bootstrap_zc_root(0, &root));
    EXPECT_EQ(root, 137u);
    ASSERT_TRUE(bootstrap_zc_root(1, &root));
    EXPECT_EQ(root, 197u);
    EXPECT_FALSE(bootstrap_zc_root(2, &root));

    const uint16_t major0[8] = {0x019D, 0x00ED, 0x01E8, 0x00E8, 0x00FB, 0x0021, 0x0054, 0x00EC};
    const uint16_t major1[8] = {0xF110, 0x3D21, 0xE550, 0xBD49, 0x23CF, 0x0B50, 0x3D3C, 0xA216};
    for (int minor = 0; minor < 8; minor++) {
        ASSERT_TRUE(bootstrap_pn_seed(0, minor, &seed));
        EXPECT_EQ(seed, major0[minor]);
        ASSERT_TRUE(bootstrap_pn_seed(1, minor, &seed));
        EXPECT_EQ(seed, major1[minor]);
    }
    EXPECT_FALSE(bootstrap_pn_seed(0, 8, &seed));
}

TEST(BootstrapSignalTest, PnSequenceMatchesFigure52) {
    for (uint16_t seed : {uint16_t{0x019D}, uint16_t{0xF110}, uint16_t{0xA216}}) {
        std::vector<int> want = BootstrapTxModel::pn(seed, 4 * 749);
        std::vector<uint8_t> got(want.size());
        bootstrap_pn_sequence(seed, got.size(), got.data());
        for (size_t k = 0; k < want.size(); k++) {
            ASSERT_EQ(got[k], want[k]) << "seed " << seed << " k " << k;
        }
    }
}

TEST(BootstrapSignalTest, ReferenceMatchesSection523) {
    for (int major : {0, 1}) {
        for (int minor : {0, 5}) {
            for (size_t n = 0; n < 4; n++) {
                std::vector<std::complex<float>> bins(kBootstrapFftSize);
                ASSERT_TRUE(bootstrap_reference(major, minor, n, bins.data()));
                std::vector<std::complex<double>> s = BootstrapTxModel::freq(
                    major == 0 ? 137 : 197, major == 0 ? (minor == 0 ? 0x019D : 0x0021)
                                                       : (minor == 0 ? 0xF110 : 0x0B50),
                    static_cast<int>(n));
                size_t active = 0;
                for (int k = -1024; k < 1024; k++) {
                    std::complex<float> got = bins[static_cast<size_t>(k + 2048) % 2048];
                    std::complex<double> want =
                        (k == 0 || k < -749 || k > 749) ? 0.0 : s[k + 749];
                    ASSERT_NEAR(got.real(), want.real(), 1e-5) << major << minor << n << k;
                    ASSERT_NEAR(got.imag(), want.imag(), 1e-5) << major << minor << n << k;
                    active += std::abs(got) > 0.5f;
                }
                EXPECT_EQ(active, 1498u);
            }
        }
    }
}

TEST(BootstrapSignalTest, GrayDemapInvertsTableB11) {
    const unsigned shifts[16] = {64,   192,  448,  320,  960,  832,  576,  704,
                                 1984, 1856, 1600, 1728, 1088, 1216, 1472, 1344};
    for (unsigned bits = 0; bits < 16; bits++) {
        EXPECT_EQ(bootstrap_gray_demap(shifts[bits], 4), bits);
        // Table B.1.2: each shift tolerates +-63 without a bit error.
        EXPECT_EQ(bootstrap_gray_demap(shifts[bits] - 63, 4), bits);
        EXPECT_EQ(bootstrap_gray_demap(shifts[bits] + 63, 4), bits);
    }
}

TEST(BootstrapSignalTest, GrayDemapInvertsSection532ForEightBits) {
    for (int v = 0; v < 256; v++) {
        std::vector<int> b(8);
        for (int i = 0; i < 8; i++) {
            b[i] = (v >> (7 - i)) & 1;
        }
        int shift = BootstrapTxModel::gray_map(b);
        for (int err = -3; err <= 3; err++) {
            unsigned est = static_cast<unsigned>((shift + err + 2048) % 2048);
            ASSERT_EQ(bootstrap_gray_demap(est, 8), static_cast<unsigned>(v)) << err;
        }
    }
}

TEST(BootstrapSignalTest, MinTimeToNextFollowsTable63) {
    const unsigned ms[31] = {50,   100,  150,  200,  250,  300,  350,  400,  500,  600,  700,
                             800,  900,  1000, 1100, 1200, 1300, 1500, 1700, 1900, 2100, 2300,
                             2500, 2700, 2900, 3300, 3700, 4100, 4500, 4900, 5300};
    for (unsigned x = 0; x < 31; x++) {
        EXPECT_EQ(bootstrap_min_time_to_next_ms(x), ms[x]) << x;
    }
}

TEST(BootstrapSignalTest, PostBootstrapRateForTable91Values) {
    EXPECT_DOUBLE_EQ(bootstrap_post_sample_rate_hz(2), 6.912e6);
    EXPECT_DOUBLE_EQ(bootstrap_post_sample_rate_hz(5), 8.064e6);
    EXPECT_DOUBLE_EQ(bootstrap_post_sample_rate_hz(8), 9.216e6);
}

TEST(BootstrapSignalTest, SignalingFieldsFollowTables62To65) {
    // ea_wake_up_1=1, min_time_to_next=10, system_bandwidth=00;
    // ea_wake_up_2=0, bsr_coefficient=2; preamble_structure=37.
    const uint8_t fields[3] = {0x80 | (10 << 2) | 0, 2, 37};
    BootstrapSignaling s;
    ASSERT_TRUE(parse_bootstrap_signaling(fields, &s));
    EXPECT_EQ(s.ea_wake_up_1, 1);
    EXPECT_EQ(s.min_time_to_next, 10);
    EXPECT_EQ(s.system_bandwidth, 0);
    EXPECT_EQ(s.ea_wake_up_2, 0);
    EXPECT_EQ(s.bsr_coefficient, 2);
    EXPECT_EQ(s.preamble_structure, 37);

    const uint8_t ea2[3] = {0x01, 0x80 | 8, 0};
    ASSERT_TRUE(parse_bootstrap_signaling(ea2, &s));
    EXPECT_EQ(s.ea_wake_up_1, 0);
    EXPECT_EQ(s.system_bandwidth, 1);
    EXPECT_EQ(s.ea_wake_up_2, 1);
    EXPECT_EQ(s.bsr_coefficient, 8);
}

TEST(BootstrapSignalTest, ReservedOrDisallowedValuesAreRejected) {
    BootstrapSignaling s;
    const uint8_t ok[3] = {0, 2, 0};
    EXPECT_TRUE(parse_bootstrap_signaling(ok, &s));
    const uint8_t min_time_31[3] = {31 << 2, 2, 0};
    EXPECT_FALSE(parse_bootstrap_signaling(min_time_31, &s));
    const uint8_t bandwidth_11[3] = {3, 2, 0};
    EXPECT_FALSE(parse_bootstrap_signaling(bandwidth_11, &s));
    for (int bsr = 0; bsr < 128; bsr++) {
        const uint8_t f[3] = {0, static_cast<uint8_t>(bsr), 0};
        EXPECT_EQ(parse_bootstrap_signaling(f, &s), bsr == 2 || bsr == 5 || bsr == 8) << bsr;
    }
    const uint8_t ps_159[3] = {0, 2, 159};
    EXPECT_TRUE(parse_bootstrap_signaling(ps_159, &s));
    const uint8_t ps_160[3] = {0, 2, 160};
    EXPECT_FALSE(parse_bootstrap_signaling(ps_160, &s));
}

struct TableH11Row {
    unsigned value;
    size_t fft;
    size_t gi;
    size_t dx;
    int mode;
};

// A/322 Table H.1.1, every row as printed.
const TableH11Row kTableH11[160] = {
    {0, 8192, 192, 16, 1}, {1, 8192, 192, 16, 2}, {2, 8192, 192, 16, 3}, {3, 8192, 192, 16, 4},
    {4, 8192, 192, 16, 5}, {5, 8192, 384, 8, 1}, {6, 8192, 384, 8, 2}, {7, 8192, 384, 8, 3},
    {8, 8192, 384, 8, 4}, {9, 8192, 384, 8, 5}, {10, 8192, 512, 6, 1}, {11, 8192, 512, 6, 2},
    {12, 8192, 512, 6, 3}, {13, 8192, 512, 6, 4}, {14, 8192, 512, 6, 5}, {15, 8192, 768, 4, 1},
    {16, 8192, 768, 4, 2}, {17, 8192, 768, 4, 3}, {18, 8192, 768, 4, 4}, {19, 8192, 768, 4, 5},
    {20, 8192, 1024, 3, 1}, {21, 8192, 1024, 3, 2}, {22, 8192, 1024, 3, 3}, {23, 8192, 1024, 3, 4},
    {24, 8192, 1024, 3, 5}, {25, 8192, 1536, 4, 1}, {26, 8192, 1536, 4, 2}, {27, 8192, 1536, 4, 3},
    {28, 8192, 1536, 4, 4}, {29, 8192, 1536, 4, 5}, {30, 8192, 2048, 3, 1}, {31, 8192, 2048, 3, 2},
    {32, 8192, 2048, 3, 3}, {33, 8192, 2048, 3, 4}, {34, 8192, 2048, 3, 5}, {35, 16384, 192, 32, 1},
    {36, 16384, 192, 32, 2}, {37, 16384, 192, 32, 3}, {38, 16384, 192, 32, 4}, {39, 16384, 192, 32, 5},
    {40, 16384, 384, 16, 1}, {41, 16384, 384, 16, 2}, {42, 16384, 384, 16, 3}, {43, 16384, 384, 16, 4},
    {44, 16384, 384, 16, 5}, {45, 16384, 512, 12, 1}, {46, 16384, 512, 12, 2}, {47, 16384, 512, 12, 3},
    {48, 16384, 512, 12, 4}, {49, 16384, 512, 12, 5}, {50, 16384, 768, 8, 1}, {51, 16384, 768, 8, 2},
    {52, 16384, 768, 8, 3}, {53, 16384, 768, 8, 4}, {54, 16384, 768, 8, 5}, {55, 16384, 1024, 6, 1},
    {56, 16384, 1024, 6, 2}, {57, 16384, 1024, 6, 3}, {58, 16384, 1024, 6, 4}, {59, 16384, 1024, 6, 5},
    {60, 16384, 1536, 4, 1}, {61, 16384, 1536, 4, 2}, {62, 16384, 1536, 4, 3}, {63, 16384, 1536, 4, 4},
    {64, 16384, 1536, 4, 5}, {65, 16384, 2048, 3, 1}, {66, 16384, 2048, 3, 2}, {67, 16384, 2048, 3, 3},
    {68, 16384, 2048, 3, 4}, {69, 16384, 2048, 3, 5}, {70, 16384, 2432, 3, 1}, {71, 16384, 2432, 3, 2},
    {72, 16384, 2432, 3, 3}, {73, 16384, 2432, 3, 4}, {74, 16384, 2432, 3, 5}, {75, 16384, 3072, 4, 1},
    {76, 16384, 3072, 4, 2}, {77, 16384, 3072, 4, 3}, {78, 16384, 3072, 4, 4}, {79, 16384, 3072, 4, 5},
    {80, 16384, 3648, 4, 1}, {81, 16384, 3648, 4, 2}, {82, 16384, 3648, 4, 3}, {83, 16384, 3648, 4, 4},
    {84, 16384, 3648, 4, 5}, {85, 16384, 4096, 3, 1}, {86, 16384, 4096, 3, 2}, {87, 16384, 4096, 3, 3},
    {88, 16384, 4096, 3, 4}, {89, 16384, 4096, 3, 5}, {90, 32768, 192, 32, 1}, {91, 32768, 192, 32, 2},
    {92, 32768, 192, 32, 3}, {93, 32768, 192, 32, 4}, {94, 32768, 192, 32, 5}, {95, 32768, 384, 32, 1},
    {96, 32768, 384, 32, 2}, {97, 32768, 384, 32, 3}, {98, 32768, 384, 32, 4}, {99, 32768, 384, 32, 5},
    {100, 32768, 512, 24, 1}, {101, 32768, 512, 24, 2}, {102, 32768, 512, 24, 3}, {103, 32768, 512, 24, 4},
    {104, 32768, 512, 24, 5}, {105, 32768, 768, 16, 1}, {106, 32768, 768, 16, 2}, {107, 32768, 768, 16, 3},
    {108, 32768, 768, 16, 4}, {109, 32768, 768, 16, 5}, {110, 32768, 1024, 12, 1}, {111, 32768, 1024, 12, 2},
    {112, 32768, 1024, 12, 3}, {113, 32768, 1024, 12, 4}, {114, 32768, 1024, 12, 5}, {115, 32768, 1536, 8, 1},
    {116, 32768, 1536, 8, 2}, {117, 32768, 1536, 8, 3}, {118, 32768, 1536, 8, 4}, {119, 32768, 1536, 8, 5},
    {120, 32768, 2048, 6, 1}, {121, 32768, 2048, 6, 2}, {122, 32768, 2048, 6, 3}, {123, 32768, 2048, 6, 4},
    {124, 32768, 2048, 6, 5}, {125, 32768, 2432, 6, 1}, {126, 32768, 2432, 6, 2}, {127, 32768, 2432, 6, 3},
    {128, 32768, 2432, 6, 4}, {129, 32768, 2432, 6, 5}, {130, 32768, 3072, 8, 1}, {131, 32768, 3072, 8, 2},
    {132, 32768, 3072, 8, 3}, {133, 32768, 3072, 8, 4}, {134, 32768, 3072, 8, 5}, {135, 32768, 3072, 3, 1},
    {136, 32768, 3072, 3, 2}, {137, 32768, 3072, 3, 3}, {138, 32768, 3072, 3, 4}, {139, 32768, 3072, 3, 5},
    {140, 32768, 3648, 8, 1}, {141, 32768, 3648, 8, 2}, {142, 32768, 3648, 8, 3}, {143, 32768, 3648, 8, 4},
    {144, 32768, 3648, 8, 5}, {145, 32768, 3648, 3, 1}, {146, 32768, 3648, 3, 2}, {147, 32768, 3648, 3, 3},
    {148, 32768, 3648, 3, 4}, {149, 32768, 3648, 3, 5}, {150, 32768, 4096, 3, 1}, {151, 32768, 4096, 3, 2},
    {152, 32768, 4096, 3, 3}, {153, 32768, 4096, 3, 4}, {154, 32768, 4096, 3, 5}, {155, 32768, 4864, 3, 1},
    {156, 32768, 4864, 3, 2}, {157, 32768, 4864, 3, 3}, {158, 32768, 4864, 3, 4}, {159, 32768, 4864, 3, 5},
};

TEST(BootstrapSignalTest, PreambleStructureFollowsTableH11) {
    for (const TableH11Row& row : kTableH11) {
        PreambleStructure p;
        ASSERT_TRUE(preamble_structure_params(row.value, &p)) << row.value;
        EXPECT_EQ(p.fft_size, row.fft) << row.value;
        EXPECT_EQ(p.guard_interval, row.gi) << row.value;
        EXPECT_EQ(p.pilot_dx, row.dx) << row.value;
        EXPECT_EQ(p.l1_basic_mode, row.mode) << row.value;
    }
    PreambleStructure p;
    for (unsigned v = 160; v < 256; v++) {
        EXPECT_FALSE(preamble_structure_params(v, &p)) << v;
    }
}

}  // namespace
}  // namespace sync
}  // namespace atsc3
