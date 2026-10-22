// test_pilot_extractor.cc — Unit tests for lib/ofdm/pilot_extractor.h
//
// Expected values come from ATSC A/322:2018, not from the implementation:
// the reference sequence (Section 8.1.2), the scattered pilot rule (8.1.3.1),
// the common CP sets (Tables D.1.2 / D.1.3, Table 8.4) and, for every
// FFT size, carrier reduction and pattern, the number of data cells per
// symbol (Tables 7.3 and 7.4), which only comes out right if the scattered,
// continual (common and additional) and edge pilot sets are all correct.

#include "pilot_extractor.h"

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

constexpr PilotPattern kAllPatterns[] = {
    PilotPattern::SP3_2,  PilotPattern::SP3_4,  PilotPattern::SP4_2,  PilotPattern::SP4_4,
    PilotPattern::SP6_2,  PilotPattern::SP6_4,  PilotPattern::SP8_2,  PilotPattern::SP8_4,
    PilotPattern::SP12_2, PilotPattern::SP12_4, PilotPattern::SP16_2, PilotPattern::SP16_4,
    PilotPattern::SP24_2, PilotPattern::SP24_4, PilotPattern::SP32_2, PilotPattern::SP32_4,
};

// Tables 7.3 and 7.4: available data cells per data symbol for each FFT size
// and Cred_coeff, one column per pattern in code order. A negative entry is
// bracketed in the standard (combination not allowed), 0 is "N/A".
struct DataCellRow {
    size_t fft_size;
    size_t cred_coeff;
    size_t noc;
    int data_cells[16];
};

constexpr DataCellRow kDataCells[] = {
    {8192,
     0,
     6913,
     {5711, 6285, 5999, 6429, 6287, 6573, 6431, 6645, 6575, 6717, 6647, 6753, -6719, -6789, 6755,
      6807}},
    {8192,
     1,
     6817,
     {5631, 6197, 5915, 6339, 6199, 6481, 6341, 6552, 6483, 6623, 6554, 6660, -6625, -6694, 6661,
      6714}},
    {8192,
     2,
     6721,
     {5552, 6110, 5832, 6250, 6112, 6390, 6252, 6460, 6392, 6530, 6462, 6565, -6532, -6600, 6567,
      6619}},
    {8192,
     3,
     6625,
     {5473, 6023, 5749, 6161, 6025, 6299, 6163, 6368, 6301, 6437, 6370, 6473, -6439, -6506, 6474,
      6524}},
    {8192,
     4,
     6529,
     {5394, 5936, 5666, 6072, 5938, 6208, 6074, 6276, 6210, 6344, 6278, 6378, -6346, -6412, 6380,
      6429}},
    {16384,
     0,
     13825,
     {11423, 12573, 11999, 12861, 12575, 13149, 12863, 13293, 13151, 13437, 13295, 13509, 13439,
      13581, 13511, 13617}},
    {16384,
     1,
     13633,
     {11263, 12397, 11831, 12681, 12399, 12965, 12683, 13107, 12967, 13249, 13109, 13320, 13251,
      13391, 13322, 13428}},
    {16384,
     2,
     13441,
     {11106, 12224, 11666, 12504, 12226, 12784, 12506, 12924, 12786, 13064, 12926, 13134, 13066,
      13204, 13136, 13239}},
    {16384,
     3,
     13249,
     {10947, 12049, 11499, 12325, 12051, 12601, 12327, 12739, 12603, 12877, 12741, 12946, 12879,
      13015, 12948, 13051}},
    {16384,
     4,
     13057,
     {10789, 11875, 11333, 12147, 11877, 12419, 12149, 12555, 12421, 12691, 12557, 12759, 12693,
      12827, 12761, 12861}},
    {32768,
     0,
     27649,
     {22847, -25149, 0, 0, 25151, -26301, 25727, -26589, 26303, -26877, 26591, -27021, 26879,
      -27165, 27023, -27237}},
    {32768,
     1,
     27265,
     {22527, -24797, 0, 0, 24799, -25933, 25367, -26217, 25935, -26501, 26219, -26643, 26503,
      -26785, 26645, -26856}},
    {32768,
     2,
     26881,
     {22213, -24451, 0, 0, 24453, -25571, 25013, -25851, 25573, -26131, 25853, -26271, 26133,
      -26411, 26273, -26481}},
    {32768,
     3,
     26497,
     {21895, -24101, 0, 0, 24103, -25205, 24655, -25481, 25207, -25757, 25483, -25895, 25759,
      -26033, 25897, -26102}},
    {32768,
     4,
     26113,
     {21579, -23753, 0, 0, 23755, -24841, 24299, -25113, 24843, -25385, 25115, -25521, 25387,
      -25657, 25523, -25725}},
};

// Table D.1.3: common CP set CP8 (absolute carrier indices)
constexpr size_t kCp8[48] = {
    59,   167,  307,  469,  637,  751,  865,  1031, 1159, 1333, 1447, 1607, 1811, 1943, 2041, 2197,
    2323, 2519, 2605, 2767, 2963, 3029, 3175, 3325, 3467, 3665, 3833, 3901, 4073, 4235, 4325, 4511,
    4627, 4825, 4907, 5051, 5227, 5389, 5531, 5627, 5833, 5905, 6053, 6197, 6353, 6563, 6637, 6809,
};

// Table D.1.2: common CP set CP16 (absolute carrier indices)
constexpr size_t kCp16[96] = {
    118,   178,   334,   434,   614,   670,   938,   1070,  1274,  1358,  1502,  1618,
    1730,  1918,  2062,  2078,  2318,  2566,  2666,  2750,  2894,  3010,  3214,  3250,
    3622,  3686,  3886,  3962,  4082,  4166,  4394,  4558,  4646,  4718,  5038,  5170,
    5210,  5342,  5534,  5614,  5926,  5942,  6058,  6134,  6350,  6410,  6650,  6782,
    6934,  7154,  7330,  7438,  7666,  7742,  7802,  7894,  8146,  8258,  8470,  8494,
    8650,  8722,  9022,  9118,  9254,  9422,  9650,  9670,  9814,  9902,  10102, 10166,
    10454, 10598, 10778, 10822, 11062, 11138, 11254, 11318, 11666, 11758, 11810, 11974,
    12106, 12242, 12394, 12502, 12706, 12866, 13126, 13190, 13274, 13466, 13618, 13666,
};

// Table 8.4: number of common continual pilots, per Cred_coeff 0..4
constexpr size_t kCommonCpCount[3][5] = {
    {48, 48, 47, 46, 45},
    {96, 96, 93, 92, 90},
    {192, 192, 186, 184, 180},
};

size_t fft_column(size_t fft_size) {
    return fft_size == 8192 ? 0 : (fft_size == 16384 ? 1 : 2);
}

PilotExtractorConfig make_config(size_t fft_size, PilotPattern pattern, size_t noc = 0) {
    PilotExtractorConfig config;
    config.fft_size = fft_size;
    config.pattern = pattern;
    config.num_active_carriers = noc;
    return config;
}

std::vector<sample_t> zero_symbol(size_t fft_size) {
    return std::vector<sample_t>(fft_size, sample_t(0, 0));
}

// Sign of a reference value: +1 for r_k = 0, -1 for r_k = 1
int reference_sign(const sample_t& value) {
    return value.real() > 0 ? 1 : -1;
}

//==============================================================================
// Construction Tests
//==============================================================================

TEST(PilotExtractorTest, DefaultConstruction) {
    PilotExtractor extractor;

    auto config = extractor.get_config();
    EXPECT_EQ(config.fft_size, 8192u);
    EXPECT_EQ(config.pattern, PilotPattern::SP6_4);
    EXPECT_EQ(config.num_active_carriers, 6913u);
}

TEST(PilotExtractorTest, PatternCodesAreTheL1SignalingValues) {
    // Table 9.12: L1D_scattered_pilot_pattern 00000 = SP3_2 ... 01111 = SP32_4
    for (size_t code = 0; code < 16; ++code) {
        EXPECT_EQ(static_cast<size_t>(kAllPatterns[code]), code);
    }
}

TEST(PilotExtractorTest, PatternSpacingTable8_2) {
    const size_t dx[16] = {3, 3, 4, 4, 6, 6, 8, 8, 12, 12, 16, 16, 24, 24, 32, 32};
    const size_t dy[16] = {2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4};
    for (size_t code = 0; code < 16; ++code) {
        // 16K allows every pattern
        auto params = get_pilot_pattern_params(kAllPatterns[code], 16384);
        EXPECT_EQ(params.dx, dx[code]) << "pattern code " << code;
        EXPECT_EQ(params.dy, dy[code]) << "pattern code " << code;
        EXPECT_EQ(params.pattern, kAllPatterns[code]);
    }
}

TEST(PilotExtractorTest, InvalidConfigurationsThrow) {
    EXPECT_THROW(PilotExtractor(make_config(4096, PilotPattern::SP6_4)), std::invalid_argument);
    EXPECT_THROW(PilotExtractor(make_config(8192, static_cast<PilotPattern>(16))),
                 std::invalid_argument);
    // Carrier counts that are not NoCmax - Cred_coeff * Cunit, Cred_coeff <= 4
    EXPECT_THROW(PilotExtractor(make_config(8192, PilotPattern::SP6_4, 6000)),
                 std::invalid_argument);
    EXPECT_THROW(PilotExtractor(make_config(8192, PilotPattern::SP6_4, 6914)),
                 std::invalid_argument);
    EXPECT_THROW(PilotExtractor(make_config(8192, PilotPattern::SP6_4, 6913 - 5 * 96)),
                 std::invalid_argument);
    EXPECT_THROW(PilotExtractor(make_config(16384, PilotPattern::SP6_4, 6913 - 96)),
                 std::invalid_argument);
}

TEST(PilotExtractorTest, DisallowedCombinationsThrowExactlyWhereTheSpecBracketsThem) {
    for (const DataCellRow& row : kDataCells) {
        if (row.cred_coeff != 0) {
            continue;
        }
        for (size_t code = 0; code < 16; ++code) {
            const bool allowed = row.data_cells[code] > 0;
            if (allowed) {
                EXPECT_NO_THROW(PilotExtractor(make_config(row.fft_size, kAllPatterns[code])))
                    << "fft " << row.fft_size << " pattern code " << code;
            } else {
                EXPECT_THROW(PilotExtractor(make_config(row.fft_size, kAllPatterns[code])),
                             std::invalid_argument)
                    << "fft " << row.fft_size << " pattern code " << code;
            }
        }
    }
}

//==============================================================================
// Reference sequence (Section 8.1.2)
//==============================================================================

TEST(PilotExtractorTest, ReferenceSequenceStartsWithTheSpecifiedValues) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    const size_t first = extractor.get_pattern_params().first_active;

    // "The first 24 values of the reference pilot sequence are
    // 1101 1000 0000 0001 0100 0000"
    const int r[24] = {1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    for (size_t k = 0; k < 24; ++k) {
        // Re{c} = 2 A (1/2 - r_k): r_k = 0 is +A, r_k = 1 is -A
        EXPECT_EQ(reference_sign(extractor.get_reference(first + k, 0)), r[k] ? -1 : 1)
            << "carrier " << k;
    }
}

TEST(PilotExtractorTest, ReferenceSequenceMatchesTheFigure8_2ShiftRegister) {
    // Independent bit-serial model of Figure 8.2: x1..x13 loaded with
    // 0000 0000 1101 1, output from x13, feedback x9^x10^x12^x13 into x1.
    int x[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1};  // x[1..13]
    std::vector<int> r;
    for (size_t n = 0; n < 27649; ++n) {
        r.push_back(x[13]);
        int fb = x[9] ^ x[10] ^ x[12] ^ x[13];
        for (int i = 13; i > 1; --i) {
            x[i] = x[i - 1];
        }
        x[1] = fb;
    }

    PilotExtractor extractor(make_config(32768, PilotPattern::SP6_2));
    const size_t first = extractor.get_pattern_params().first_active;
    for (size_t k = 0; k < r.size(); ++k) {
        ASSERT_EQ(reference_sign(extractor.get_reference(first + k, 0)), r[k] ? -1 : 1)
            << "carrier " << k;
    }
}

TEST(PilotExtractorTest, ReferenceSequenceRestartsEverySymbolAtTheFirstActiveCarrier) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    const size_t first = extractor.get_pattern_params().first_active;
    for (size_t sym = 0; sym < 5; ++sym) {
        EXPECT_EQ(reference_sign(extractor.get_reference(first, sym)), -1) << "symbol " << sym;
        EXPECT_EQ(reference_sign(extractor.get_reference(first + 2, sym)), 1) << "symbol " << sym;
    }
}

TEST(PilotExtractorTest, ReferenceValuesAreBipolarReal) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    auto symbol = zero_symbol(8192);
    for (const auto& pilot : extractor.extract(symbol.data(), 0)) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_EQ(std::abs(pilot.reference_value.real()), 32767);
#else
        EXPECT_EQ(std::abs(pilot.reference_value.real()), 1.0f);
#endif
        EXPECT_EQ(pilot.reference_value.imag(), 0);
    }
}

//==============================================================================
// Pilot locations
//==============================================================================

TEST(PilotExtractorTest, ScatteredPilotsFollowSection8_1_3_1) {
    // k mod (DX*DY) == DX * (l mod DY) on relative carrier k
    for (PilotPattern pattern : {PilotPattern::SP6_4, PilotPattern::SP12_2, PilotPattern::SP3_4}) {
        PilotExtractor extractor(make_config(8192, pattern));
        const auto params = extractor.get_pattern_params();
        for (size_t sym = 0; sym < 9; ++sym) {
            size_t count = 0;
            for (size_t k = 0; k < 6913; ++k) {
                const bool expected =
                    (k % (params.dx * params.dy)) == params.dx * (sym % params.dy);
                ASSERT_EQ(extractor.is_scattered_pilot(params.first_active + k, sym), expected)
                    << "relative carrier " << k << " symbol " << sym;
                count += expected ? 1 : 0;
            }
            EXPECT_EQ(extractor.get_scattered_pilot_count(sym), count);
        }
    }
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    const auto params = extractor.get_pattern_params();
    EXPECT_FALSE(extractor.is_scattered_pilot(params.first_active - 1, 0));
    EXPECT_FALSE(extractor.is_scattered_pilot(params.last_active + 1, 0));
}

TEST(PilotExtractorTest, EdgePilotsAreRelativeCarriers0AndNocMinus1) {
    for (size_t noc : {6913u, 6817u, 6529u}) {
        PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4, noc));
        const auto params = extractor.get_pattern_params();
        EXPECT_EQ(params.last_active - params.first_active + 1, noc);
        EXPECT_TRUE(extractor.is_pilot(params.first_active, 1));
        EXPECT_TRUE(extractor.is_pilot(params.last_active, 1));

        auto symbol = zero_symbol(8192);
        auto edge = extractor.extract_edge(symbol.data());
        ASSERT_EQ(edge.size(), 2u);
        EXPECT_EQ(edge[0].subcarrier_index, params.first_active);
        EXPECT_EQ(edge[1].subcarrier_index, params.last_active);
        for (const auto& pilot : edge) {
            EXPECT_EQ(pilot.type, PilotType::EDGE);
        }
    }
}

TEST(PilotExtractorTest, CommonContinualPilotsMatchTablesD1_2AndD1_3) {
    // SP3_2 adds one scattered-pilot-bearing CP (Table D.1.4): 1731 at 8K,
    // 3471 at 16K. Carrier reduction 0 puts relative = absolute index.
    for (size_t fft : {8192u, 16384u}) {
        PilotExtractor extractor(make_config(fft, PilotPattern::SP3_2));
        const size_t first = extractor.get_pattern_params().first_active;
        auto symbol = zero_symbol(fft);
        std::set<size_t> got;
        for (const auto& pilot : extractor.extract_continual(symbol.data())) {
            got.insert(pilot.subcarrier_index - first);
        }
        std::set<size_t> want;
        if (fft == 8192) {
            want.insert(kCp8, kCp8 + 48);
            want.insert(1731);
        } else {
            want.insert(kCp16, kCp16 + 96);
            want.insert(3471);
        }
        EXPECT_EQ(got, want) << "fft " << fft;
    }
}

TEST(PilotExtractorTest, Cp32IsSymmetricAndHas192Entries) {
    // Section D.1: CP32,R is the mirror image of CP32,L about carrier 13824
    PilotExtractor extractor(make_config(32768, PilotPattern::SP3_2));
    const size_t first = extractor.get_pattern_params().first_active;
    auto symbol = zero_symbol(32768);
    std::vector<size_t> cp;
    for (const auto& pilot : extractor.extract_continual(symbol.data())) {
        if (pilot.subcarrier_index - first != 6939) {  // the additional CP
            cp.push_back(pilot.subcarrier_index - first);
        }
    }
    ASSERT_EQ(cp.size(), 192u);
    EXPECT_EQ(cp.front(), 236u);
    EXPECT_EQ(cp.back(), 27412u);
    for (size_t k = 0; k < 96; ++k) {
        EXPECT_EQ(cp[191 - k], 27648 - cp[k]) << "k " << k;
    }
}

TEST(PilotExtractorTest, Cp32EntriesAreMultiplesOfFour) {
    // So the ceiling in the 16K / 8K derivation (Section 8.1.4.1) is always
    // exact; the RTL relies on this and shifts.
    for (size_t entry : common_continual_pilots(32768)) {
        EXPECT_EQ(entry % 4, 0u) << "entry " << entry;
    }
}

TEST(PilotExtractorTest, CommonContinualPilotCountsMatchTable8_4) {
    // SP3_2 has exactly one additional CP at every FFT size and reduction
    // (index 1731 / 3471 / 6939 is below NoC and not in parentheses)
    for (size_t fft : {8192u, 16384u, 32768u}) {
        const size_t col = fft_column(fft);
        const size_t noc_max = (fft == 8192 ? 6913 : (fft == 16384 ? 13825 : 27649));
        const size_t c_unit = 96u << col;
        for (size_t cred = 0; cred <= 4; ++cred) {
            PilotExtractor extractor(
                make_config(fft, PilotPattern::SP3_2, noc_max - cred * c_unit));
            EXPECT_EQ(extractor.get_continual_pilot_count(), kCommonCpCount[col][cred] + 1)
                << "fft " << fft << " Cred_coeff " << cred;
        }
    }
}

TEST(PilotExtractorTest, CarrierReductionShiftsTheContinualPilots) {
    // 8K, Cred_coeff 2: absolute carriers run from Cred_coeff * Cunit / 2 =
    // 96, so CP8 entry 59 falls outside and 167 becomes relative carrier 71.
    PilotExtractor extractor(make_config(8192, PilotPattern::SP3_2, 6913 - 2 * 96));
    const size_t first = extractor.get_pattern_params().first_active;
    auto symbol = zero_symbol(8192);
    auto cp = extractor.extract_continual(symbol.data());
    ASSERT_FALSE(cp.empty());
    EXPECT_EQ(cp.front().subcarrier_index - first, 71u);
    EXPECT_EQ(extractor.get_pattern_params().reduced_carriers, 2u);
}

TEST(PilotExtractorTest, ParenthesizedAdditionalPilotsAreDroppedForOddReduction) {
    // 16K SP32_4: (5824) and (11488) are used only for even Cred_coeff;
    // 8K SP32_4 (Table D.1.5) has no additional CP at Cred_coeff 1.
    auto additional = [](size_t fft, PilotPattern pattern, size_t cred) {
        const size_t noc_max = fft == 8192 ? 6913 : 13825;
        const size_t c_unit = fft == 8192 ? 96 : 192;
        PilotExtractor extractor(make_config(fft, pattern, noc_max - cred * c_unit));
        const auto params = extractor.get_pattern_params();
        auto symbol = zero_symbol(fft);
        std::vector<size_t> out;
        for (const auto& pilot : extractor.extract_continual(symbol.data())) {
            const size_t rel = pilot.subcarrier_index - params.first_active;
            if (rel == 3488 || rel == 5824 || rel == 11488 || rel == 1696 || rel == 2880 ||
                rel == 5728) {
                out.push_back(rel);
            }
        }
        return out;
    };
    EXPECT_EQ(additional(16384, PilotPattern::SP32_4, 0), (std::vector<size_t>{3488, 5824, 11488}));
    EXPECT_EQ(additional(16384, PilotPattern::SP32_4, 1), (std::vector<size_t>{3488}));
    EXPECT_EQ(additional(16384, PilotPattern::SP32_4, 2), (std::vector<size_t>{3488, 5824, 11488}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_4, 0), (std::vector<size_t>{1696, 2880, 5728}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_4, 1), (std::vector<size_t>{}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_4, 2), (std::vector<size_t>{1696}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_4, 3), (std::vector<size_t>{1696, 2880}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_4, 4), (std::vector<size_t>{1696, 2880, 5728}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_2, 0), (std::vector<size_t>{1696}));
    EXPECT_EQ(additional(8192, PilotPattern::SP32_2, 1), (std::vector<size_t>{}));
}

// The additional CPs exist "to ensure a constant number of data carriers in
// every data symbol": every symbol of the pattern, for every pattern, FFT
// size and carrier reduction the standard allows, leaves exactly the number
// of data cells Tables 7.3 and 7.4 give.
TEST(PilotExtractorTest, DataCellsPerSymbolMatchTables7_3And7_4) {
    size_t checked = 0;
    for (const DataCellRow& row : kDataCells) {
        for (size_t code = 0; code < 16; ++code) {
            if (row.data_cells[code] <= 0) {
                continue;
            }
            PilotExtractor extractor(make_config(row.fft_size, kAllPatterns[code], row.noc));
            const auto params = extractor.get_pattern_params();
            ASSERT_EQ(params.last_active - params.first_active + 1, row.noc);
            auto symbol = zero_symbol(row.fft_size);
            for (size_t sym = 0; sym < params.dy; ++sym) {
                const size_t pilots = extractor.extract(symbol.data(), sym).size();
                ASSERT_EQ(row.noc - pilots, static_cast<size_t>(row.data_cells[code]))
                    << "fft " << row.fft_size << " Cred_coeff " << row.cred_coeff
                    << " pattern code " << code << " symbol " << sym;
            }
            ++checked;
        }
    }
    EXPECT_EQ(checked, 185u);
}

//==============================================================================
// Extraction
//==============================================================================

TEST(PilotExtractorTest, ExtractAllPilots) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));

    std::vector<sample_t> fft_output(8192);
    for (size_t i = 0; i < 8192; ++i) {
#ifdef ATSC3_FIXED_POINT
        fft_output[i] = sample_t(float_to_q15(0.5f), float_to_q15(0.25f));
#else
        fft_output[i] = sample_t(0.5f, 0.25f);
#endif
    }

    auto pilots = extractor.extract(fft_output.data(), 0);
    EXPECT_GT(pilots.size(), 50u);

    for (const auto& pilot : pilots) {
#ifdef ATSC3_FIXED_POINT
        EXPECT_NEAR(q15_to_float(pilot.received_value.real()), 0.5f, 1e-3f);
        EXPECT_NEAR(q15_to_float(pilot.received_value.imag()), 0.25f, 1e-3f);
#else
        EXPECT_FLOAT_EQ(pilot.received_value.real(), 0.5f);
        EXPECT_FLOAT_EQ(pilot.received_value.imag(), 0.25f);
#endif
    }
}

TEST(PilotExtractorTest, ExtractSubsetsHaveTheirOwnTypes) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    auto symbol = zero_symbol(8192);

    auto scattered = extractor.extract_scattered(symbol.data(), 0);
    for (const auto& pilot : scattered) {
        EXPECT_EQ(pilot.type, PilotType::SCATTERED);
    }
    EXPECT_EQ(scattered.size(), extractor.get_scattered_pilot_count(0));

    auto continual = extractor.extract_continual(symbol.data());
    for (const auto& pilot : continual) {
        EXPECT_EQ(pilot.type, PilotType::CONTINUAL);
        EXPECT_TRUE(extractor.is_continual_pilot(pilot.subcarrier_index));
    }
    EXPECT_EQ(continual.size(), extractor.get_continual_pilot_count());
}

TEST(PilotExtractorTest, ExtractedReferenceFollowsTheRelativeCarrierIndex) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    const size_t first = extractor.get_pattern_params().first_active;
    auto symbol = zero_symbol(8192);
    for (const auto& pilot : extractor.extract(symbol.data(), 2)) {
        EXPECT_EQ(pilot.reference_value, extractor.get_reference(pilot.subcarrier_index, 2));
    }
    // The edge pilot on relative carrier 0 uses r_0 = 1: -A
    EXPECT_EQ(reference_sign(extractor.get_reference(first, 0)), -1);
}

//==============================================================================
// Channel Estimation Tests
//==============================================================================

TEST(PilotSymbolTest, EstimateChannelUnity) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = +1+j0 -> Channel = +1+j0
#ifdef ATSC3_FIXED_POINT
    // Use 32767 (max Q15) instead of float_to_q15(1.0f) to avoid overflow
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 1.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 0.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 1.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 0.0f, 1e-6f);
#endif
}

TEST(PilotSymbolTest, EstimateChannelWithPhase) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0+j1 -> Channel = 0+j1 (90 deg rotation)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, 32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 0.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 1.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 0.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 1.0f, 1e-6f);
#endif
}

TEST(PilotSymbolTest, EstimateChannelNegativeRef) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = -1, Received = -1+j0 -> Channel = +1+j0
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(-32767, 0);
    pilot.received_value = sample_t(-32767, 0);
#else
    pilot.reference_value = sample_t(-1.0f, 0.0f);
    pilot.received_value = sample_t(-1.0f, 0.0f);
#endif

    auto h = pilot.estimate_channel();

#ifdef ATSC3_FIXED_POINT
    EXPECT_NEAR(q15_to_float(h.real()), 1.0f, 0.01f);
    EXPECT_NEAR(q15_to_float(h.imag()), 0.0f, 0.01f);
#else
    EXPECT_NEAR(h.real(), 1.0f, 1e-6f);
    EXPECT_NEAR(h.imag(), 0.0f, 1e-6f);
#endif
}

//==============================================================================
// Phase Error Tests
//==============================================================================

TEST(PilotSymbolTest, PhaseErrorZero) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = +1+j0 -> No phase error
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(32767, 0);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(1.0f, 0.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, 0.0, 1e-6);
}

TEST(PilotSymbolTest, PhaseError90Degrees) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0+j1 -> 90 degree phase error (pi/2)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, 32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, 1.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, M_PI / 2.0, 1e-5);
}

TEST(PilotSymbolTest, PhaseErrorMinus90Degrees) {
    PilotSymbol pilot;
    pilot.subcarrier_index = 100;

    // Reference = +1, Received = 0-j1 -> -90 degree phase error (-pi/2)
#ifdef ATSC3_FIXED_POINT
    pilot.reference_value = sample_t(32767, 0);
    pilot.received_value = sample_t(0, -32767);
#else
    pilot.reference_value = sample_t(1.0f, 0.0f);
    pilot.received_value = sample_t(0.0f, -1.0f);
#endif

    double error = pilot.get_phase_error();
    EXPECT_NEAR(error, -M_PI / 2.0, 1e-5);
}

//==============================================================================
// Reconfiguration Tests
//==============================================================================

TEST(PilotExtractorTest, Reconfigure) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));

    auto params1 = extractor.get_pattern_params();
    EXPECT_EQ(params1.dx, 6u);
    EXPECT_EQ(params1.dy, 4u);

    extractor.set_config(make_config(8192, PilotPattern::SP3_4));

    auto params2 = extractor.get_pattern_params();
    EXPECT_EQ(params2.dx, 3u);
    EXPECT_EQ(params2.dy, 4u);
}

TEST(PilotExtractorTest, ReconfigureFftSize) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    const size_t cp_count_8k = extractor.get_continual_pilot_count();

    extractor.set_config(make_config(16384, PilotPattern::SP6_4));
    EXPECT_GT(extractor.get_continual_pilot_count(), cp_count_8k);
}

//==============================================================================
// Sorted and Unique Output Test
//==============================================================================

TEST(PilotExtractorTest, OutputIsSortedAndUnique) {
    PilotExtractor extractor(make_config(8192, PilotPattern::SP6_4));
    auto symbol = zero_symbol(8192);
    auto pilots = extractor.extract(symbol.data(), 0);

    for (size_t i = 1; i < pilots.size(); ++i) {
        EXPECT_LT(pilots[i - 1].subcarrier_index, pilots[i].subcarrier_index)
            << "Pilots not sorted at index " << i;
    }
}

// A carrier that is both a scattered and a continual (or edge) pilot is
// reported once, as SCATTERED; a continual pilot on an edge carrier is
// reported as CONTINUAL. The RTL port classifies each carrier with this
// same fixed precedence, so it must not depend on sort stability.
TEST(PilotExtractorTest, CollisionPrecedenceScatteredContinualEdge) {
    size_t sp_cp_collisions = 0;
    size_t sp_edge_collisions = 0;
    for (const DataCellRow& row : kDataCells) {
        if (row.cred_coeff != 0) {
            continue;
        }
        auto symbol = zero_symbol(row.fft_size);
        for (size_t code = 0; code < 16; ++code) {
            if (row.data_cells[code] <= 0) {
                continue;
            }
            PilotExtractor extractor(make_config(row.fft_size, kAllPatterns[code]));
            PilotPatternParams params = extractor.get_pattern_params();
            for (size_t sym = 0; sym < params.dy; ++sym) {
                for (const PilotSymbol& p : extractor.extract(symbol.data(), sym)) {
                    size_t k = p.subcarrier_index;
                    bool sp = extractor.is_scattered_pilot(k, sym);
                    bool cp = extractor.is_continual_pilot(k);
                    bool edge = (k == params.first_active || k == params.last_active);
                    PilotType expected = sp   ? PilotType::SCATTERED
                                         : cp ? PilotType::CONTINUAL
                                              : PilotType::EDGE;
                    ASSERT_TRUE(sp || cp || edge) << "non-pilot carrier " << k;
                    ASSERT_EQ(p.type, expected) << "fft " << row.fft_size << " pattern code "
                                                << code << " sym " << sym << " carrier " << k;
                    sp_cp_collisions += (sp && cp) ? 1 : 0;
                    sp_edge_collisions += (sp && edge) ? 1 : 0;
                }
            }
        }
    }
    // Non-vacuity: both kinds of collision occur (not in every configuration).
    EXPECT_GT(sp_cp_collisions, 0u);
    EXPECT_GT(sp_edge_collisions, 0u);
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
