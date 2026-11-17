// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_preamble.cc — Unit tests for lib/ofdm/preamble.h
//
// Expected values are A/322:2026 Tables 7.1 (NoC), 7.2 (data cells per
// preamble symbol, every FFT size / GI / Dx row of Table H.1.1 and every
// Cred_coeff), 8.4 (common continual pilots) and 8.6 (pilot power).

#include "ofdm/preamble.h"

#include <gtest/gtest.h>

namespace atsc3 {
namespace ofdm {
namespace {

TEST(PreambleTest, NumCarriersTable71) {
    const size_t want[3][5] = {{6913, 6817, 6721, 6625, 6529},
                               {13825, 13633, 13441, 13249, 13057},
                               {27649, 27265, 26881, 26497, 26113}};
    const size_t ffts[3] = {8192, 16384, 32768};
    for (int f = 0; f < 3; ++f) {
        for (unsigned c = 0; c < 5; ++c) {
            EXPECT_EQ(num_carriers(ffts[f], c), want[f][c]) << ffts[f] << " " << c;
        }
        EXPECT_EQ(num_carriers(ffts[f], 5), 0u);
    }
    EXPECT_EQ(num_carriers(4096, 0), 0u);
}

// Table 7.2, all rows (GI listed for reference; the layout depends on the
// FFT size and Dx only).
TEST(PreambleTest, DataCellsTable72) {
    struct Row {
        size_t fft, gi, dx;
        size_t cells[5];
    };
    const Row rows[] = {
        {8192, 192, 16, {6432, 6342, 6253, 6164, 6075}},
        {8192, 384, 8, {6000, 5916, 5833, 5750, 5667}},
        {8192, 512, 6, {5712, 5632, 5553, 5474, 5395}},
        {8192, 768, 4, {5136, 5064, 4993, 4922, 4851}},
        {8192, 1024, 3, {4560, 4496, 4433, 4370, 4307}},
        {8192, 1536, 4, {5136, 5064, 4993, 4922, 4851}},
        {8192, 2048, 3, {4560, 4496, 4433, 4370, 4307}},
        {16384, 192, 32, {13296, 13110, 12927, 12742, 12558}},
        {16384, 384, 16, {12864, 12684, 12507, 12328, 12150}},
        {16384, 512, 12, {12576, 12400, 12227, 12052, 11878}},
        {16384, 768, 8, {12000, 11832, 11667, 11500, 11334}},
        {16384, 1024, 6, {11424, 11264, 11107, 10948, 10790}},
        {16384, 1536, 4, {10272, 10128, 9987, 9844, 9702}},
        {16384, 2048, 3, {9120, 8992, 8867, 8740, 8614}},
        {16384, 2432, 3, {9120, 8992, 8867, 8740, 8614}},
        {16384, 3072, 4, {10272, 10128, 9987, 9844, 9702}},
        {16384, 3648, 4, {10272, 10128, 9987, 9844, 9702}},
        {16384, 4096, 3, {9120, 8992, 8867, 8740, 8614}},
        {32768, 192, 32, {26592, 26220, 25854, 25484, 25116}},
        {32768, 384, 32, {26592, 26220, 25854, 25484, 25116}},
        {32768, 512, 24, {26304, 25936, 25574, 25208, 24844}},
        {32768, 768, 16, {25728, 25368, 25014, 24656, 24300}},
        {32768, 1024, 12, {25152, 24800, 24454, 24104, 23756}},
        {32768, 1536, 8, {24000, 23664, 23334, 23000, 22668}},
        {32768, 2048, 6, {22848, 22528, 22214, 21896, 21580}},
        {32768, 2432, 6, {22848, 22528, 22214, 21896, 21580}},
        {32768, 3072, 8, {24000, 23664, 23334, 23000, 22668}},
        {32768, 3072, 3, {18240, 17984, 17734, 17480, 17228}},
        {32768, 3648, 8, {24000, 23664, 23334, 23000, 22668}},
        {32768, 3648, 3, {18240, 17984, 17734, 17480, 17228}},
        {32768, 4096, 3, {18240, 17984, 17734, 17480, 17228}},
        {32768, 4864, 3, {18240, 17984, 17734, 17480, 17228}},
    };
    for (const Row& r : rows) {
        for (unsigned c = 0; c < 5; ++c) {
            PreambleLayout l;
            ASSERT_TRUE(preamble_layout(r.fft, r.dx, c, &l)) << r.fft << " " << r.dx;
            EXPECT_EQ(l.data.size(), r.cells[c])
                << r.fft << " GI " << r.gi << " Dx " << r.dx << " Cred " << c;
            EXPECT_EQ(l.data.size() + l.pilots.size() + l.continual.size(), l.noc);
        }
    }
}

// Table 8.4: the common CPs inside NoC, none on a preamble pilot.
TEST(PreambleTest, ContinualPilotsTable84) {
    const size_t want[5][3] = {{48, 96, 192},
                               {48, 96, 192},
                               {47, 93, 186},
                               {46, 92, 184},
                               {45, 90, 180}};
    const size_t ffts[3] = {8192, 16384, 32768};
    const size_t dxs[3] = {3, 3, 3};
    for (unsigned c = 0; c < 5; ++c) {
        for (int f = 0; f < 3; ++f) {
            PreambleLayout l;
            ASSERT_TRUE(preamble_layout(ffts[f], dxs[f], c, &l));
            EXPECT_EQ(l.continual.size(), want[c][f]) << ffts[f] << " Cred " << c;
        }
    }
}

// Section 8.1.4.1: common CPs avoid scattered pilot positions, so none
// falls on a preamble pilot (k mod Dx == 0) for any pair in Table H.1.1.
TEST(PreambleTest, ContinualPilotsNeverOnPreamblePilots) {
    const size_t pairs[][2] = {{8192, 16},  {8192, 8},   {8192, 6},   {8192, 4},   {8192, 3},
                               {16384, 32}, {16384, 16}, {16384, 12}, {16384, 8},  {16384, 6},
                               {16384, 4},  {16384, 3},  {32768, 32}, {32768, 24}, {32768, 16},
                               {32768, 12}, {32768, 8},  {32768, 6},  {32768, 3}};
    for (const auto& p : pairs) {
        for (unsigned c = 0; c < 5; ++c) {
            PreambleLayout l;
            ASSERT_TRUE(preamble_layout(p[0], p[1], c, &l));
            for (uint32_t k : l.continual) {
                ASSERT_NE(k % p[1], 0u) << p[0] << " Dx " << p[1] << " Cred " << c << " k " << k;
            }
        }
    }
}

TEST(PreambleTest, PilotsEveryDx) {
    PreambleLayout l;
    ASSERT_TRUE(preamble_layout(8192, 16, 4, &l));
    EXPECT_EQ(l.noc, 6529u);
    ASSERT_FALSE(l.pilots.empty());
    for (size_t i = 0; i < l.pilots.size(); ++i) {
        EXPECT_EQ(l.pilots[i], 16 * i);
    }
    for (size_t i = 1; i < l.data.size(); ++i) {
        ASSERT_LT(l.data[i - 1], l.data[i]);
    }
}

// Pairs Table H.1.1 does not use are rejected.
TEST(PreambleTest, RejectsUnusedPairs) {
    PreambleLayout l;
    EXPECT_FALSE(preamble_layout(8192, 32, 0, &l));
    EXPECT_FALSE(preamble_layout(16384, 24, 0, &l));
    EXPECT_FALSE(preamble_layout(32768, 4, 0, &l));
    EXPECT_FALSE(preamble_layout(8192, 3, 5, &l));
    EXPECT_FALSE(preamble_layout(4096, 3, 0, &l));
}

TEST(PreambleTest, PilotPowerTable86) {
    double db = 0.0;
    ASSERT_TRUE(preamble_pilot_power_db(8192, 16, &db));
    EXPECT_DOUBLE_EQ(db, 5.30);
    ASSERT_TRUE(preamble_pilot_power_db(16384, 8, &db));
    EXPECT_DOUBLE_EQ(db, 3.60);
    ASSERT_TRUE(preamble_pilot_power_db(32768, 8, &db));
    EXPECT_DOUBLE_EQ(db, 4.00);
    ASSERT_TRUE(preamble_pilot_power_db(32768, 3, &db));
    EXPECT_DOUBLE_EQ(db, 1.30);
    EXPECT_FALSE(preamble_pilot_power_db(8192, 32, &db));
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
