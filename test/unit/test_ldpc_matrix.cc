// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_ldpc_matrix.cc — Unit tests for ATSC 3.0 LDPC H matrix generation
//
// Tests H matrix dimensions, sparsity, and structure per ATSC A/322
// Section 6.1.3 and Annex A.

#include "fec/ldpc_decoder.h"
#include "fec/ldpc_matrix.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>

namespace atsc3 {
namespace fec {
namespace {

class LdpcMatrixTest : public ::testing::Test {
protected:
    LdpcMatrixGenerator generator_;
};

//------------------------------------------------------------------------------
// Dimension Tests - Long Codes (N=64800, Q=360)
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, LongCode_Rate2_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_2_15, false);

    EXPECT_EQ(H.num_cols, 64800);                   // Codeword length
    EXPECT_EQ(H.num_rows, 64800 - 8640);            // Parity bits = 56160
    EXPECT_EQ(H.info_bits, 8640);                   // Info bits
    EXPECT_EQ(H.row_indices.size(), H.num_rows);    // Row storage
    EXPECT_EQ(H.col_indices.size(), H.num_cols);    // Column storage
}

TEST_F(LdpcMatrixTest, LongCode_Rate7_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, false);

    EXPECT_EQ(H.num_cols, 64800);
    EXPECT_EQ(H.num_rows, 64800 - 30240);  // 34560 parity
    EXPECT_EQ(H.info_bits, 30240);
}

TEST_F(LdpcMatrixTest, LongCode_Rate13_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_13_15, false);

    EXPECT_EQ(H.num_cols, 64800);
    EXPECT_EQ(H.num_rows, 64800 - 56160);  // 8640 parity
    EXPECT_EQ(H.info_bits, 56160);
}

//------------------------------------------------------------------------------
// Dimension Tests - Short Codes (N=16200, 45 groups of 360)
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, ShortCode_Rate2_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_2_15, true);

    EXPECT_EQ(H.num_cols, 16200);
    EXPECT_EQ(H.num_rows, 16200 - 2160);  // 14040 parity
    EXPECT_EQ(H.info_bits, 2160);
}

TEST_F(LdpcMatrixTest, ShortCode_Rate7_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, true);

    EXPECT_EQ(H.num_cols, 16200);
    EXPECT_EQ(H.num_rows, 16200 - 7560);  // 8640 parity
    EXPECT_EQ(H.info_bits, 7560);
}

TEST_F(LdpcMatrixTest, ShortCode_Rate13_15_Dimensions) {
    auto H = generator_.generate(config::CodeRate::RATE_13_15, true);

    EXPECT_EQ(H.num_cols, 16200);
    EXPECT_EQ(H.num_rows, 16200 - 14040);  // 2160 parity
    EXPECT_EQ(H.info_bits, 14040);
}

//------------------------------------------------------------------------------
// All Code Rates Test
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, AllCodeRates_LongCodes) {
    const config::CodeRate rates[] = {
        config::CodeRate::RATE_2_15,  config::CodeRate::RATE_3_15,  config::CodeRate::RATE_4_15,
        config::CodeRate::RATE_5_15,  config::CodeRate::RATE_6_15,  config::CodeRate::RATE_7_15,
        config::CodeRate::RATE_8_15,  config::CodeRate::RATE_9_15,  config::CodeRate::RATE_10_15,
        config::CodeRate::RATE_11_15, config::CodeRate::RATE_12_15, config::CodeRate::RATE_13_15,
    };

    for (auto rate : rates) {
        auto H = generator_.generate(rate, false);
        EXPECT_TRUE(generator_.validate_matrix(H, rate, false))
            << "Failed for rate index " << static_cast<int>(rate);
        EXPECT_EQ(H.num_cols, 64800);
        EXPECT_GT(H.num_rows, 0);
        EXPECT_TRUE(H.is_valid());
    }
}

TEST_F(LdpcMatrixTest, AllCodeRates_ShortCodes) {
    const config::CodeRate rates[] = {
        config::CodeRate::RATE_2_15,  config::CodeRate::RATE_3_15,  config::CodeRate::RATE_4_15,
        config::CodeRate::RATE_5_15,  config::CodeRate::RATE_6_15,  config::CodeRate::RATE_7_15,
        config::CodeRate::RATE_8_15,  config::CodeRate::RATE_9_15,  config::CodeRate::RATE_10_15,
        config::CodeRate::RATE_11_15, config::CodeRate::RATE_12_15, config::CodeRate::RATE_13_15,
    };

    for (auto rate : rates) {
        auto H = generator_.generate(rate, true);
        EXPECT_TRUE(generator_.validate_matrix(H, rate, true))
            << "Failed for rate index " << static_cast<int>(rate);
        EXPECT_EQ(H.num_cols, 16200);
        EXPECT_GT(H.num_rows, 0);
        EXPECT_TRUE(H.is_valid());
    }
}

//------------------------------------------------------------------------------
// Sparsity Tests
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, Sparsity_RowConnectivity) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, false);

    // Every row should have at least one connection
    for (size_t row = 0; row < H.num_rows; ++row) {
        EXPECT_GT(H.row_indices[row].size(), 0) << "Row " << row << " has no connections";
    }

    // Average row weight should be reasonable (typically 6-30 for LDPC)
    size_t total_edges = 0;
    for (const auto& row : H.row_indices) {
        total_edges += row.size();
    }
    double avg_row_weight = static_cast<double>(total_edges) / H.num_rows;
    EXPECT_GE(avg_row_weight, 2.0);
    EXPECT_LE(avg_row_weight, 50.0);
}

TEST_F(LdpcMatrixTest, Sparsity_ColumnConnectivity) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, false);

    // Count columns with connections
    size_t connected_cols = 0;
    for (const auto& col : H.col_indices) {
        if (!col.empty()) {
            ++connected_cols;
        }
    }

    // Most columns should be connected
    EXPECT_GT(connected_cols, H.num_cols / 2);
}

TEST_F(LdpcMatrixTest, Sparsity_EdgesConsistent) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, true);

    // Count edges from rows and columns - should match
    size_t row_edges = 0;
    for (const auto& row : H.row_indices) {
        row_edges += row.size();
    }

    size_t col_edges = 0;
    for (const auto& col : H.col_indices) {
        col_edges += col.size();
    }

    EXPECT_EQ(row_edges, col_edges);
}

//------------------------------------------------------------------------------
// A/322 Annex A structure
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, GroupSizeIs360ForBothLengths) {
    // A/322 Section 6.2.2: Ngroup = Ninner / 360 for both code lengths
    EXPECT_EQ(LdpcMatrixGenerator::get_expansion_factor(false), 360);
    EXPECT_EQ(LdpcMatrixGenerator::get_expansion_factor(true), 360);
}

std::vector<uint16_t> sorted(std::vector<uint16_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

TEST_F(LdpcMatrixTest, TypeB_InfoColumnsFollowTableA_1_12) {
    // Table A.1.12 (13/15, 64800) first row; Type B, Qldpc = 24, Minner = 8640
    const std::vector<uint16_t> row0 = {142,  2307, 2598, 2650, 4028, 4434, 5781,
                                        5881, 6016, 6323, 6681, 6698, 8125};
    auto H = generator_.generate(config::CodeRate::RATE_13_15, false);
    EXPECT_EQ(H.col_indices[0], row0);

    std::vector<uint16_t> col1;
    for (uint16_t x : row0) {
        col1.push_back(static_cast<uint16_t>((x + 24) % 8640));
    }
    EXPECT_EQ(H.col_indices[1], sorted(col1));
}

TEST_F(LdpcMatrixTest, TypeA_InfoColumnsFollowTableA_2_1) {
    // Table A.2.1 (2/15, 16200) first row; Type A, M1 = 3240 (Q1 = 9),
    // M2 = 10800 (Q2 = 30), Table 6.6
    auto H = generator_.generate(config::CodeRate::RATE_2_15, true);
    EXPECT_EQ(H.col_indices[0], (std::vector<uint16_t>{2889, 3122, 3208, 4324, 5968, 7241, 13215}));
    EXPECT_EQ(H.col_indices[1], (std::vector<uint16_t>{2898, 3131, 3217, 4354, 5998, 7271, 13245}));
}

TEST_F(LdpcMatrixTest, TypeA_ParityColumnsAreInterleaved) {
    // Section 6.1.3.1 step vi: codeword bit Nouter + 360 t + s carries p_{Q1 s + t}.
    // For 2/15 at 16200 (Nouter = 2160, Q1 = 9), p_1 sits at column 2160 + 360,
    // and check 1 (p_1 = a_1 + p_0) touches p_0 at column 2160 and p_1 there.
    auto H = generator_.generate(config::CodeRate::RATE_2_15, true);
    const auto& check1 = H.row_indices[1];
    EXPECT_TRUE(std::binary_search(check1.begin(), check1.end(), 2160));
    EXPECT_TRUE(std::binary_search(check1.begin(), check1.end(), 2160 + 360));
    // M2 parity is the identity part: check 3240 + 0 holds p_3240 at column
    // 2160 + 3240 and no other M2 parity column.
    const auto& check_m2 = H.row_indices[3240];
    size_t m2_cols = 0;
    for (uint16_t c : check_m2) {
        m2_cols += c >= 2160 + 3240 ? 1 : 0;
    }
    EXPECT_EQ(m2_cols, 1u);
    EXPECT_TRUE(std::binary_search(check_m2.begin(), check_m2.end(), 2160 + 3240));
}

size_t unsatisfied_checks(const SparseMatrix& H, const std::vector<uint8_t>& cw) {
    size_t unsatisfied = 0;
    for (const auto& row : H.row_indices) {
        uint8_t parity = 0;
        for (uint16_t col : row) {
            parity ^= cw[col];
        }
        unsatisfied += parity;
    }
    return unsatisfied;
}

TEST_F(LdpcMatrixTest, EncodedCodewordsSatisfyEveryCheck) {
    // The encoder follows the transmitter procedure of Section 6.1.3; H is built
    // from the check equations. Random information must give a zero syndrome
    // for all 24 codes.
    std::mt19937 rng(1234);
    for (bool short_cw : {false, true}) {
        for (int r = 0; r <= static_cast<int>(config::CodeRate::RATE_13_15); ++r) {
            auto rate = static_cast<config::CodeRate>(r);
            auto H = generator_.generate(rate, short_cw);
            ASSERT_TRUE(generator_.validate_matrix(H, rate, short_cw));
            std::vector<uint8_t> info(H.info_bits);
            for (auto& b : info) {
                b = static_cast<uint8_t>(rng() & 1);
            }
            std::vector<uint8_t> cw(H.num_cols, 0xFF);
            ASSERT_TRUE(ldpc_encode(rate, short_cw, info.data(), cw.data()));
            for (uint8_t b : cw) {
                ASSERT_LE(b, 1) << "codeword bit left unwritten";
            }
            EXPECT_EQ(unsatisfied_checks(H, cw), 0u)
                << "rate index " << r << (short_cw ? " short" : " long");
        }
    }
}

TEST_F(LdpcMatrixTest, InvalidRateGivesEmptyMatrix) {
    auto H = generator_.generate(static_cast<config::CodeRate>(12), false);
    EXPECT_FALSE(H.is_valid());
    std::vector<uint8_t> buf(64800, 0);
    EXPECT_FALSE(ldpc_encode(static_cast<config::CodeRate>(12), false, buf.data(), buf.data()));
}

//------------------------------------------------------------------------------
// Syndrome Check Tests
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, Syndrome_AllZeroCodeword) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, true);

    // All-zero codeword should satisfy all parity checks
    std::vector<uint8_t> codeword(H.num_cols, 0);

    // Compute syndrome
    size_t unsatisfied = 0;
    for (size_t row = 0; row < H.num_rows; ++row) {
        uint8_t parity = 0;
        for (uint16_t col : H.row_indices[row]) {
            parity ^= codeword[col];
        }
        if (parity != 0) {
            ++unsatisfied;
        }
    }

    EXPECT_EQ(unsatisfied, 0);
}

TEST_F(LdpcMatrixTest, Syndrome_SingleBitFlip) {
    auto H = generator_.generate(config::CodeRate::RATE_7_15, true);

    // Start with all-zero codeword
    std::vector<uint8_t> codeword(H.num_cols, 0);

    // Flip one bit
    codeword[0] = 1;

    // Should have some unsatisfied checks (those connected to bit 0)
    size_t unsatisfied = 0;
    for (size_t row = 0; row < H.num_rows; ++row) {
        uint8_t parity = 0;
        for (uint16_t col : H.row_indices[row]) {
            parity ^= codeword[col];
        }
        if (parity != 0) {
            ++unsatisfied;
        }
    }

    // Number of unsatisfied checks equals column weight of flipped bit
    EXPECT_EQ(unsatisfied, H.col_indices[0].size());
}

//------------------------------------------------------------------------------
// Integration with Decoder
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, Decoder_UsesGeneratedMatrix) {
    LdpcConfig config;
    config.code_rate = config::CodeRate::RATE_7_15;
    config.short_codeword = true;
    config.max_iterations = 10;

    LdpcDecoder decoder(config);

    // Check decoder has proper matrix
    const auto& H = decoder.get_parity_matrix();
    EXPECT_TRUE(H.is_valid());
    EXPECT_EQ(H.num_cols, 16200);
    EXPECT_EQ(H.info_bits, 7560);
}

TEST_F(LdpcMatrixTest, Decoder_AllZeroInput_Converges) {
    LdpcConfig config;
    config.code_rate = config::CodeRate::RATE_7_15;
    config.short_codeword = true;
    config.max_iterations = 50;

    LdpcDecoder decoder(config);

    // All-zero LLRs (representing all-zero codeword with high confidence)
    std::vector<int8_t> llr_in(16200, 10);  // Positive = bit 0 likely

    LdpcResult result = decoder.decode(llr_in.data());

    // Should converge quickly for clean input
    EXPECT_TRUE(result.is_valid);
    EXPECT_EQ(result.num_unsatisfied_checks, 0);

    // All decoded bits should be 0
    for (uint8_t bit : result.decoded_bits) {
        EXPECT_EQ(bit, 0);
    }
}

class LdpcNoisyDecodeTest : public ::testing::TestWithParam<bool> {};

TEST_P(LdpcNoisyDecodeTest, RecoversRandomCodeword) {
    // A random (non-zero) codeword exercises the actual code structure; the
    // all-zero codeword satisfies any H. LLRs reach the int8 limit, so the
    // decoder's internal sums must not be clamped to that range.
    LdpcConfig config;
    config.code_rate = config::CodeRate::RATE_7_15;
    config.short_codeword = true;
    config.max_iterations = 50;
    config.use_fixed_point = GetParam();
    LdpcDecoder decoder(config);

    std::mt19937 rng(77);
    std::vector<uint8_t> info(7560);
    for (auto& b : info) {
        b = static_cast<uint8_t>(rng() & 1);
    }
    std::vector<uint8_t> cw(16200);
    ASSERT_TRUE(ldpc_encode(config.code_rate, true, info.data(), cw.data()));

    // BPSK, sigma 0.8 (about 1.9 dB Es/N0); LLR = 40 y, so strong bits saturate int8
    std::normal_distribution<float> noise(0.0f, 0.8f);
    std::vector<int8_t> llr(cw.size());
    size_t raw_errors = 0;
    for (size_t i = 0; i < cw.size(); ++i) {
        float y = (cw[i] ? -1.0f : 1.0f) + noise(rng);
        float l = std::clamp(40.0f * y, -127.0f, 127.0f);
        llr[i] = static_cast<int8_t>(l);
        raw_errors += (y < 0) != (cw[i] != 0) ? 1 : 0;
    }
    ASSERT_GT(raw_errors, 100u);  // the channel really corrupts the word

    LdpcResult result = decoder.decode(llr.data());
    EXPECT_TRUE(result.converged);
    EXPECT_EQ(result.decoded_bits, info);
}

INSTANTIATE_TEST_SUITE_P(FloatAndFixed, LdpcNoisyDecodeTest, ::testing::Bool());

//------------------------------------------------------------------------------
// File Path Tests
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, FilePath_Format) {
    std::string path = get_ldpc_matrix_path(config::CodeRate::RATE_7_15, false);
    EXPECT_EQ(path, "config/ldpc_tables/ldpc_long_7_15.json");

    path = get_ldpc_matrix_path(config::CodeRate::RATE_2_15, true);
    EXPECT_EQ(path, "config/ldpc_tables/ldpc_short_2_15.json");
}

}  // namespace
}  // namespace fec
}  // namespace atsc3
