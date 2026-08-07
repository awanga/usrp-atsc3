// test_ldpc_matrix.cc — Unit tests for ATSC 3.0 LDPC H matrix generation
//
// Tests H matrix dimensions, sparsity, and structure per ATSC A/322 Section 9.

#include <gtest/gtest.h>

#include "fec/ldpc_decoder.h"
#include "fec/ldpc_matrix.h"

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
// Dimension Tests - Short Codes (N=16200, Q=90)
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
// Quasi-Cyclic Structure Tests
//------------------------------------------------------------------------------

TEST_F(LdpcMatrixTest, QuasiCyclic_ExpansionFactor) {
    EXPECT_EQ(LdpcMatrixGenerator::get_expansion_factor(false), 360);  // Long codes
    EXPECT_EQ(LdpcMatrixGenerator::get_expansion_factor(true), 90);    // Short codes
}

TEST_F(LdpcMatrixTest, QuasiCyclic_BaseMatrix_Valid) {
    auto base = generator_.get_base_matrix(config::CodeRate::RATE_7_15, false);

    EXPECT_TRUE(base.is_valid());
    EXPECT_EQ(base.expansion_factor, 360);
    EXPECT_EQ(base.num_block_cols, 180);  // 64800 / 360
    EXPECT_GT(base.num_block_rows, 0);

    // Check entries are in valid range
    for (const auto& row : base.entries) {
        for (int16_t val : row) {
            EXPECT_GE(val, -1);
            EXPECT_LT(val, static_cast<int16_t>(base.expansion_factor));
        }
    }
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
