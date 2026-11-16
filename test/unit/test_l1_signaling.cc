// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_l1_signaling.cc — Unit tests for lib/framing/l1_signaling.h
//
// Bit strings are built here field by field in the order and widths of
// A/322 Tables 9.2 and 9.9, with the Section 6.1.2.2 CRC-32 appended, and
// the parser must return every value.

#include "fec/bch_decoder.h"
#include "framing/l1_signaling.h"

#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

class Writer {
public:
    void put(uint32_t v, size_t n) {
        for (size_t i = n; i-- > 0;) {
            bits.push_back(static_cast<uint8_t>((v >> i) & 1));
        }
    }
    void finish(size_t total_without_crc, uint8_t reserved = 0) {
        while (bits.size() < total_without_crc) {
            bits.push_back(reserved);
        }
        put(fec::outer_crc32(bits.data(), bits.size()), 32);
    }
    std::vector<uint8_t> bits;
};

std::vector<uint8_t> make_basic(uint8_t frame_length_mode, uint8_t num_subframes,
                                uint8_t time_info_flag, uint16_t detail_bytes,
                                uint8_t first_sub_mimo_mixed = 0) {
    Writer w;
    w.put(1, 3);                  // L1B_version
    w.put(0, 1);                  // mimo_scattered_pilot_encoding
    w.put(1, 1);                  // lls_flag
    w.put(time_info_flag, 2);
    w.put(0, 1);                  // return_channel_flag
    w.put(2, 2);                  // papr_reduction
    w.put(frame_length_mode, 1);
    if (frame_length_mode == 0) {
        w.put(777, 10);           // frame_length
        w.put(4321, 13);          // excess_samples_per_symbol
    } else {
        w.put(65000, 16);         // time_offset
        w.put(99, 7);             // additional_samples
    }
    w.put(num_subframes, 8);
    w.put(2, 3);                  // preamble_num_symbols
    w.put(5, 3);                  // preamble_reduced_carriers
    w.put(3, 2);                  // L1_Detail_content_tag
    w.put(detail_bytes, 13);
    w.put(2, 3);                  // L1_Detail_fec_type (Mode 3)
    w.put(1, 2);                  // additional_parity_mode
    w.put(300000, 19);            // L1_Detail_total_cells
    w.put(0, 1);                  // first_sub_mimo
    w.put(1, 2);                  // first_sub_miso
    w.put(2, 2);                  // first_sub_fft_size
    w.put(4, 3);                  // first_sub_reduced_carriers
    w.put(9, 4);                  // first_sub_guard_interval
    w.put(1234, 11);              // first_sub_num_ofdm_symbols
    w.put(17, 5);                 // first_sub_scattered_pilot_pattern
    w.put(3, 3);                  // first_sub_scattered_pilot_boost
    w.put(1, 1);                  // first_sub_sbs_first
    w.put(0, 1);                  // first_sub_sbs_last
    w.put(first_sub_mimo_mixed, 1);
    EXPECT_EQ(w.bits.size(), 121u);
    w.finish(168);                // + 47 reserved bits, CRC
    EXPECT_EQ(w.bits.size(), kL1BasicBits);
    return w.bits;
}

TEST(L1BasicTest, ParsesEveryField) {
    auto bits = make_basic(0, 1, 3, 40);
    L1Basic b;
    ASSERT_TRUE(parse_l1_basic(bits.data(), &b));
    EXPECT_EQ(b.version, 1);
    EXPECT_EQ(b.lls_flag, 1);
    EXPECT_EQ(b.time_info_flag, 3);
    EXPECT_EQ(b.papr_reduction, 2);
    EXPECT_EQ(b.frame_length, 777);
    EXPECT_EQ(b.excess_samples_per_symbol, 4321);
    EXPECT_EQ(b.num_subframes, 1);
    EXPECT_EQ(b.preamble_num_symbols, 2);
    EXPECT_EQ(b.preamble_reduced_carriers, 5);
    EXPECT_EQ(b.l1_detail_content_tag, 3);
    EXPECT_EQ(b.l1_detail_size_bytes, 40);
    EXPECT_EQ(b.l1_detail_fec_type, 2);
    EXPECT_EQ(b.l1_detail_additional_parity_mode, 1);
    EXPECT_EQ(b.l1_detail_total_cells, 300000u);
    EXPECT_EQ(b.first_sub_miso, 1);
    EXPECT_EQ(b.first_sub_fft_size, 2);
    EXPECT_EQ(b.first_sub_reduced_carriers, 4);
    EXPECT_EQ(b.first_sub_guard_interval, 9);
    EXPECT_EQ(b.first_sub_num_ofdm_symbols, 1234);
    EXPECT_EQ(b.first_sub_scattered_pilot_pattern, 17);
    EXPECT_EQ(b.first_sub_scattered_pilot_boost, 3);
    EXPECT_EQ(b.first_sub_sbs_first, 1);
    EXPECT_EQ(b.first_sub_sbs_last, 0);
}

TEST(L1BasicTest, TimeOffsetModeAndCrcFailure) {
    auto bits = make_basic(1, 0, 0, 25);
    L1Basic b;
    ASSERT_TRUE(parse_l1_basic(bits.data(), &b));
    EXPECT_EQ(b.time_offset, 65000);
    EXPECT_EQ(b.additional_samples, 99);
    bits[57] ^= 1;
    EXPECT_FALSE(parse_l1_basic(bits.data(), &b));
}

// Two Subframes; PLPs cover each TI mode, an Enhanced (layer 1) PLP,
// channel bonding and a dispersed PLP.
// Version 1 fills L1D_reserved with ones, so a parser that wrongly reads the
// version-2 loop there sees L1D_mimo_mixed = 1.
std::vector<uint8_t> make_detail(const L1Basic& basic, uint8_t version, bool mimo_mixed_sub1) {
    Writer w;
    w.put(version, 4);
    w.put(1, 3);           // num_rf
    w.put(0xBEEF, 16);     // bonded_bsid
    w.put(0, 3);           // reserved
    w.put(123456, 32);     // time_sec (time_info_flag = 3)
    w.put(456, 10);
    w.put(789, 10);
    w.put(321, 10);
    for (int i = 0; i <= basic.num_subframes; i++) {
        if (i > 0) {
            w.put(0, 1);   // mimo
            w.put(0, 2);   // miso
            w.put(1, 2);   // fft_size
            w.put(0, 3);
            w.put(5, 4);   // guard_interval
            w.put(99, 11); // num_ofdm_symbols
            w.put(3, 5);
            w.put(1, 3);
            w.put(0, 1);   // sbs_first
            w.put(1, 1);   // sbs_last
        }
        w.put(1, 1);       // subframe_multiplex (num_subframes > 0)
        w.put(1, 1);       // frequency_interleaver
        w.put(100 + i, 13); // sbs_null_cells (sub 0: sbs_first; sub 1: sbs_last)
        const int num_plp = i == 0 ? 3 : 0;
        w.put(static_cast<uint32_t>(num_plp), 6);
        for (int j = 0; j <= num_plp; j++) {
            const int ti_mode = i == 0 ? (j < 3 ? j : 0) : 1;
            const int layer = (i == 0 && j == 3) ? 1 : 0;
            w.put(static_cast<uint32_t>(10 * i + j), 6);  // plp_id
            w.put(j == 0, 1);
            w.put(static_cast<uint32_t>(layer), 2);
            w.put(1000u * static_cast<uint32_t>(j + 1), 24);
            w.put(50000u + static_cast<uint32_t>(j), 24);
            w.put(0, 2);
            w.put(1, 4);       // fec_type: BCH + 64K
            const uint32_t mod = (j == 1 || j == 2) ? 0 : 2;  // QPSK: extended-TI flag present
            w.put(mod, 4);
            w.put(7, 4);       // cod
            w.put(static_cast<uint32_t>(ti_mode), 2);
            if (ti_mode == 0) {
                w.put(12345, 15);
            } else if (ti_mode == 1) {
                w.put(3000000, 22);
            }
            w.put(j == 2 ? 1 : 0, 3);  // num_channel_bonded (num_rf > 0)
            if (j == 2) {
                w.put(1, 2);
                w.put(0, 3);
                w.put(1, 3);
            }
            if (layer == 0) {
                w.put(j == 0 ? 1 : 0, 1);  // type
                if (j == 0) {
                    w.put(300, 14);
                    w.put(70000, 24);
                }
                if ((ti_mode == 1 || ti_mode == 2) && mod == 0) {
                    w.put(1, 1);           // TI_extended_interleaving
                }
                if (ti_mode == 1) {
                    w.put(3, 3);
                    w.put(1000, 11);
                } else if (ti_mode == 2) {
                    w.put(1, 1);           // HTI_inter_subframe
                    w.put(2, 4);           // num_ti_blocks
                    w.put(400, 12);
                    for (int k = 0; k <= 2; k++) {
                        w.put(static_cast<uint32_t>(100 + k), 12);
                    }
                    w.put(1, 1);
                }
            } else {
                w.put(17, 5);              // ldm_injection_level
            }
        }
    }
    w.put(0x1234, 16);  // bsid
    if (version >= 2) {
        for (int i = 0; i <= basic.num_subframes; i++) {
            if (i > 0) {
                w.put(mimo_mixed_sub1, 1);
            }
            bool mixed = i == 0 ? basic.first_sub_mimo_mixed : mimo_mixed_sub1;
            if (mixed) {
                const size_t nplp = i == 0 ? 4 : 1;
                for (size_t j = 0; j < nplp; j++) {
                    w.put(1, 1);  // plp_mimo
                    w.put(1, 1);
                    w.put(0, 1);
                    w.put(1, 1);
                }
            }
        }
    }
    size_t total = 8 * basic.l1_detail_size_bytes;
    EXPECT_LE(w.bits.size() + 32, total);
    w.finish(total - 32, version == 1 ? 1 : 0);
    return w.bits;
}

void check_detail(const L1Detail& d) {
    EXPECT_EQ(d.num_rf, 1);
    ASSERT_EQ(d.bonded_bsid.size(), 1u);
    EXPECT_EQ(d.bonded_bsid[0], 0xBEEF);
    EXPECT_EQ(d.time_sec, 123456u);
    EXPECT_EQ(d.time_msec, 456);
    EXPECT_EQ(d.time_usec, 789);
    EXPECT_EQ(d.time_nsec, 321);
    ASSERT_EQ(d.subframes.size(), 2u);
    const auto& s0 = d.subframes[0];
    EXPECT_EQ(s0.fft_size, 2);  // from L1-Basic
    EXPECT_EQ(s0.sbs_null_cells, 100);
    ASSERT_EQ(s0.plps.size(), 4u);
    EXPECT_EQ(s0.plps[0].type, 1);
    EXPECT_EQ(s0.plps[0].num_subslices, 300);
    EXPECT_EQ(s0.plps[0].subslice_interval, 70000u);
    EXPECT_EQ(s0.plps[0].fec_block_start, 12345);
    EXPECT_EQ(s0.plps[1].ti_mode, 1);
    EXPECT_EQ(s0.plps[1].cti_fec_block_start, 3000000u);
    EXPECT_EQ(s0.plps[1].ti_extended_interleaving, 1);
    EXPECT_EQ(s0.plps[1].cti_depth, 3);
    EXPECT_EQ(s0.plps[1].cti_start_row, 1000);
    EXPECT_EQ(s0.plps[2].ti_mode, 2);
    EXPECT_EQ(s0.plps[2].num_channel_bonded, 1);
    EXPECT_EQ(s0.plps[2].bonded_rf_id, (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(s0.plps[2].hti_inter_subframe, 1);
    EXPECT_EQ(s0.plps[2].hti_num_fec_blocks_max, 400);
    EXPECT_EQ(s0.plps[2].hti_num_fec_blocks, (std::vector<uint16_t>{100, 101, 102}));
    EXPECT_EQ(s0.plps[2].hti_cell_interleaver, 1);
    EXPECT_EQ(s0.plps[2].ti_extended_interleaving, 1);
    EXPECT_EQ(s0.plps[3].layer, 1);
    EXPECT_EQ(s0.plps[3].ldm_injection_level, 17);
    EXPECT_EQ(s0.plps[3].start, 4000u);
    EXPECT_EQ(s0.plps[3].size, 50003u);
    const auto& s1 = d.subframes[1];
    EXPECT_EQ(s1.fft_size, 1);
    EXPECT_EQ(s1.guard_interval, 5);
    EXPECT_EQ(s1.num_ofdm_symbols, 99);
    EXPECT_EQ(s1.sbs_last, 1);
    EXPECT_EQ(s1.sbs_null_cells, 101);
    ASSERT_EQ(s1.plps.size(), 1u);
    EXPECT_EQ(s1.plps[0].id, 10);
    EXPECT_EQ(s1.plps[0].cti_depth, 3);
    EXPECT_EQ(d.bsid, 0x1234);
}

TEST(L1DetailTest, ParsesVersion1) {
    auto bb = make_basic(0, 1, 3, 120);
    L1Basic basic;
    ASSERT_TRUE(parse_l1_basic(bb.data(), &basic));
    auto bits = make_detail(basic, 1, false);
    L1Detail d;
    ASSERT_TRUE(parse_l1_detail(bits.data(), bits.size(), basic, &d));
    EXPECT_EQ(d.version, 1);
    check_detail(d);
    EXPECT_EQ(d.subframes[1].mimo_mixed, 0);
}

TEST(L1DetailTest, ParsesVersion2MimoMixedLoop) {
    auto bb = make_basic(0, 1, 3, 120, 1);
    L1Basic basic;
    ASSERT_TRUE(parse_l1_basic(bb.data(), &basic));
    auto bits = make_detail(basic, 2, true);
    L1Detail d;
    ASSERT_TRUE(parse_l1_detail(bits.data(), bits.size(), basic, &d));
    check_detail(d);
    EXPECT_EQ(d.subframes[1].mimo_mixed, 1);
    EXPECT_EQ(d.subframes[1].plps[0].mimo, 1);
    EXPECT_EQ(d.subframes[1].plps[0].mimo_ph, 1);
    EXPECT_EQ(d.subframes[0].mimo_mixed, 1);
    EXPECT_EQ(d.subframes[0].plps[3].mimo, 1);
}

TEST(L1DetailTest, RejectsCrcErrorAndOverrun) {
    auto bb = make_basic(0, 1, 3, 120);
    L1Basic basic;
    ASSERT_TRUE(parse_l1_basic(bb.data(), &basic));
    auto bits = make_detail(basic, 1, false);
    L1Detail d;
    bits[100] ^= 1;
    EXPECT_FALSE(parse_l1_detail(bits.data(), bits.size(), basic, &d));
    bits[100] ^= 1;
    // Truncated to 40 bytes with a CRC that matches the truncated contents:
    // the fields no longer fit, which must fail on its own.
    std::vector<uint8_t> shortbits(bits.begin(), bits.begin() + 288);
    uint32_t crc = fec::outer_crc32(shortbits.data(), shortbits.size());
    for (int b = 31; b >= 0; b--) {
        shortbits.push_back(static_cast<uint8_t>((crc >> b) & 1));
    }
    EXPECT_FALSE(parse_l1_detail(shortbits.data(), shortbits.size(), basic, &d));
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
