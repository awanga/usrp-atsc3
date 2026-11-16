// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_time_deinterleaver.cc — Unit tests for lib/ofdm/time_deinterleaver.h
//
// References written from A/322 Section 7.1: the CTI as described in
// 7.1.4.1 (line k of k delay elements; shift right, output the right-most,
// write the left-most; commutators step one line per cell), the TI Block
// sizing of 7.1.5.1, and the Twisted Block Interleaver write/read of
// 7.1.5.4 with virtual FEC Blocks. Table 9.26 and a hand-worked TBI read
// order are checked directly.

#include "ofdm/time_deinterleaver.h"

#include <deque>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

namespace atsc3 {
namespace ofdm {
namespace {

TEST(CtiTest, RowsFollowTable926) {
    EXPECT_EQ(cti_num_rows(0, false), 512u);
    EXPECT_EQ(cti_num_rows(1, false), 724u);
    EXPECT_EQ(cti_num_rows(2, false), 887u);
    EXPECT_EQ(cti_num_rows(3, false), 1024u);
    EXPECT_EQ(cti_num_rows(2, true), 1254u);
    EXPECT_EQ(cti_num_rows(3, true), 1448u);
    for (uint8_t code = 4; code < 8; code++) {
        EXPECT_EQ(cti_num_rows(code, false), 0u);
    }
}

// Transmitter CTI of Section 7.1.4.1.
class SpecCti {
public:
    explicit SpecCti(size_t rows) : lines_(rows), row_(0) {
        for (size_t k = 0; k < rows; k++) {
            lines_[k].assign(k, -1);  // initial contents (not data)
        }
    }
    int64_t push(int64_t g) {
        auto& line = lines_[row_];
        int64_t out = g;
        if (!line.empty()) {
            out = line.back();  // right-most element leaves
            line.pop_back();    // others shift right
            line.push_front(g); // input to the left-most element
        }
        row_ = (row_ + 1) % lines_.size();
        return out;
    }
    size_t row() const {
        return row_;
    }

private:
    std::vector<std::deque<int64_t>> lines_;
    size_t row_;
};

class CtiRoundTrip : public ::testing::TestWithParam<size_t> {};

INSTANTIATE_TEST_SUITE_P(Rows, CtiRoundTrip, ::testing::Values(3u, 7u, 512u, 724u));

TEST_P(CtiRoundTrip, RecoversInputAfterDelay) {
    const size_t rows = GetParam();
    SpecCti tx(rows);
    CtiDeinterleaver<int64_t> rx;
    ASSERT_TRUE(rx.configure(rows));
    ASSERT_EQ(rx.delay(), rows * (rows - 1));
    // The receiver joins mid-stream at a Subframe start, where the
    // transmitter signals its commutator row (L1D_plp_CTI_start_row).
    const size_t skip = 3 * rows + 2;
    for (size_t q = 0; q < skip; q++) {
        tx.push(static_cast<int64_t>(q));
    }
    rx.set_commutator(tx.row());
    const size_t n = rx.delay() + 5 * rows;
    std::vector<int64_t> sent(n), recv(n);
    for (size_t q = 0; q < n; q++) {
        sent[q] = tx.push(static_cast<int64_t>(skip + q));
    }
    rx.process(sent.data(), recv.data(), n);
    for (size_t q = rx.delay(); q < n; q++) {
        ASSERT_EQ(recv[q], static_cast<int64_t>(skip + q - rx.delay())) << "cell " << q;
    }
}

TEST(CtiTest, RejectsDegenerateRows) {
    CtiDeinterleaver<int> rx;
    EXPECT_FALSE(rx.configure(1));
    EXPECT_FALSE(rx.configure(0));
}

TEST(HtiTest, TiBlockSizesFollowSection7151) {
    // NBLOCKS_IF = 10 over N_TI = 3: smaller TI Blocks first.
    EXPECT_EQ(hti_fec_blocks_in_ti_block(10, 3, 0), 3u);
    EXPECT_EQ(hti_fec_blocks_in_ti_block(10, 3, 1), 3u);
    EXPECT_EQ(hti_fec_blocks_in_ti_block(10, 3, 2), 4u);
    EXPECT_EQ(hti_fec_blocks_in_ti_block(9, 3, 2), 3u);
    EXPECT_EQ(hti_fec_blocks_in_ti_block(7, 1, 0), 7u);
    EXPECT_EQ(hti_fec_blocks_ti_max(10, 3), 4u);
    EXPECT_EQ(hti_fec_blocks_ti_max(9, 3), 3u);
}

// Section 7.1.5.4 formulas worked by hand for Nr = 4, Nc = 3.
TEST(HtiTest, TwistedReadOrderHandExample) {
    std::vector<uint32_t> pos(12);
    hti_tbi_read_positions(4, 3, 3, pos.data());
    EXPECT_EQ(pos, (std::vector<uint32_t>{0, 5, 10, 3, 4, 9, 2, 7, 8, 1, 6, 11}));
    // One virtual column (theta < 4 skipped).
    std::vector<uint32_t> pos2(8);
    hti_tbi_read_positions(4, 3, 2, pos2.data());
    EXPECT_EQ(pos2, (std::vector<uint32_t>{5, 10, 4, 9, 7, 8, 6, 11}));
}

// Transmitter TBI of Section 7.1.5.4: virtual columns first, FEC Blocks
// written column-wise, read diagonal-wise skipping virtual cells.
std::vector<int64_t> spec_tbi(const std::vector<int64_t>& d, size_t nr, size_t nc,
                              size_t nfec) {
    const size_t diff = nc - nfec;
    std::vector<std::vector<int64_t>> mem(nr, std::vector<int64_t>(nc, -1));
    for (size_t r = 0; r < nfec; r++) {
        for (size_t q = 0; q < nr; q++) {
            mem[q][diff + r] = d[r * nr + q];
        }
    }
    std::vector<int64_t> out;
    for (size_t i = 0; i < nr * nc; i++) {
        size_t ri = i % nr;
        size_t ti = ri % nc;
        size_t ci = (ti + i / nr) % nc;
        if (nr * ci + ri >= diff * nr) {
            out.push_back(mem[ri][ci]);
        }
    }
    return out;
}

class HtiRoundTrip : public ::testing::TestWithParam<std::tuple<size_t, size_t, bool>> {};

INSTANTIATE_TEST_SUITE_P(Blocks, HtiRoundTrip,
                         ::testing::Combine(::testing::Values(2025u, 10800u),
                                            ::testing::Values(1u, 3u, 4u),
                                            ::testing::Bool()));

TEST_P(HtiRoundTrip, RecoversFecBlocks) {
    auto [ncells, nfec, cell_il] = GetParam();
    const size_t max = 4;
    HtiDeinterleaver<int64_t> rx;
    ASSERT_TRUE(rx.configure({ncells, max, cell_il, 1}));
    std::vector<int64_t> g(nfec * ncells);
    std::iota(g.begin(), g.end(), 1000);
    // Cell Interleaver (optional): d_{r,q} = g_{r,L_r(q)}, using the
    // library's L_0/P (checked against the spec in test_cell_deinterleaver).
    std::vector<int64_t> d(g);
    if (cell_il) {
        CellDeinterleaver ci({ncells});
        for (size_t r = 0; r < nfec; r++) {
            for (size_t q = 0; q < ncells; q++) {
                d[r * ncells + q] = g[r * ncells + (ci.basic_permutation(q) + ci.shift(r)) % ncells];
            }
        }
    }
    auto sent = spec_tbi(d, ncells, max, nfec);
    ASSERT_EQ(sent.size(), g.size());
    std::vector<int64_t> out(g.size());
    ASSERT_TRUE(rx.deinterleave_ti_block(sent.data(), nfec, out.data()));
    EXPECT_EQ(out, g);
}

TEST(HtiTest, RejectsInterSubframeAndOversizedBlocks) {
    HtiDeinterleaver<int> rx;
    EXPECT_FALSE(rx.configure({2025, 4, false, 2}));
    ASSERT_TRUE(rx.configure({2025, 4, false, 1}));
    std::vector<int> in(5 * 2025), out(5 * 2025);
    EXPECT_FALSE(rx.deinterleave_ti_block(in.data(), 5, out.data()));
    EXPECT_FALSE(rx.deinterleave_ti_block(in.data(), 0, out.data()));
}

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
