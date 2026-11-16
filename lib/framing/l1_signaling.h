// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// L1 signaling fields — A/322 Sections 9.2 (L1-Basic) and 9.3 (L1-Detail)
//
// Parses the decoded (descrambled, BCH-corrected) L1-Basic and L1-Detail
// bit strings into their fields per Tables 9.2 and 9.9, and checks the
// trailing CRC-32 of Section 6.1.2.2. Values are kept as signaled (e.g.
// L1B_num_subframes is one less than the number of Subframes); the tables
// that map codes to meanings (FFT size, guard interval, pilot pattern, ...)
// are applied by the consumers.
//
// L1-Detail is parsed per L1D_version: version 2 (A/322:2026) adds the
// per-Subframe L1D_mimo_mixed loop after L1D_bsid; for version 1 that space
// is L1D_reserved.
//
// Bits are one per byte, first transmitted bit first.
//
// Reference: ATSC A/322 Section 9, Tables 9.2 and 9.9

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace framing {

constexpr size_t kL1BasicBits = 200;

struct L1Basic {
    uint8_t version = 0;
    uint8_t mimo_scattered_pilot_encoding = 0;
    uint8_t lls_flag = 0;
    uint8_t time_info_flag = 0;
    uint8_t return_channel_flag = 0;
    uint8_t papr_reduction = 0;
    uint8_t frame_length_mode = 0;
    uint16_t frame_length = 0;               // frame_length_mode = 0
    uint16_t excess_samples_per_symbol = 0;  // frame_length_mode = 0
    uint16_t time_offset = 0;                // frame_length_mode = 1
    uint8_t additional_samples = 0;          // frame_length_mode = 1
    uint8_t num_subframes = 0;               // Subframes - 1
    uint8_t preamble_num_symbols = 0;        // Preamble symbols - 1
    uint8_t preamble_reduced_carriers = 0;
    uint8_t l1_detail_content_tag = 0;
    uint16_t l1_detail_size_bytes = 0;
    uint8_t l1_detail_fec_type = 0;  // Mode - 1 (Table 9.6)
    uint8_t l1_detail_additional_parity_mode = 0;
    uint32_t l1_detail_total_cells = 0;
    uint8_t first_sub_mimo = 0;
    uint8_t first_sub_miso = 0;
    uint8_t first_sub_fft_size = 0;
    uint8_t first_sub_reduced_carriers = 0;
    uint8_t first_sub_guard_interval = 0;
    uint16_t first_sub_num_ofdm_symbols = 0;  // symbols - 1
    uint8_t first_sub_scattered_pilot_pattern = 0;
    uint8_t first_sub_scattered_pilot_boost = 0;
    uint8_t first_sub_sbs_first = 0;
    uint8_t first_sub_sbs_last = 0;
    uint8_t first_sub_mimo_mixed = 0;
    uint32_t crc = 0;
};

struct L1DetailPlp {
    uint8_t id = 0;
    uint8_t lls_flag = 0;
    uint8_t layer = 0;
    uint32_t start = 0;
    uint32_t size = 0;
    uint8_t scrambler_type = 0;
    uint8_t fec_type = 0;
    uint8_t mod = 0;  // fec_type in 0..5
    uint8_t cod = 0;
    uint8_t ti_mode = 0;
    uint16_t fec_block_start = 0;      // ti_mode 00
    uint32_t cti_fec_block_start = 0;  // ti_mode 01
    uint8_t num_channel_bonded = 0;
    uint8_t channel_bonding_format = 0;
    std::vector<uint8_t> bonded_rf_id;
    uint8_t mimo_stream_combining = 0;
    uint8_t mimo_iq_interleaving = 0;
    uint8_t mimo_ph = 0;
    uint8_t type = 0;  // layer 0
    uint16_t num_subslices = 0;
    uint32_t subslice_interval = 0;
    uint8_t ti_extended_interleaving = 0;
    uint8_t cti_depth = 0;
    uint16_t cti_start_row = 0;
    uint8_t hti_inter_subframe = 0;
    uint8_t hti_num_ti_blocks = 0;
    uint16_t hti_num_fec_blocks_max = 0;
    std::vector<uint16_t> hti_num_fec_blocks;
    uint8_t hti_cell_interleaver = 0;
    uint8_t ldm_injection_level = 0;  // layer > 0
    // L1D_version >= 2, when the Subframe is MIMO-mixed
    uint8_t mimo = 0;
};

struct L1DetailSubframe {
    // Subframe 0 takes these from L1-Basic (first_sub_*).
    uint8_t mimo = 0;
    uint8_t miso = 0;
    uint8_t fft_size = 0;
    uint8_t reduced_carriers = 0;
    uint8_t guard_interval = 0;
    uint16_t num_ofdm_symbols = 0;
    uint8_t scattered_pilot_pattern = 0;
    uint8_t scattered_pilot_boost = 0;
    uint8_t sbs_first = 0;
    uint8_t sbs_last = 0;
    uint8_t subframe_multiplex = 0;
    uint8_t frequency_interleaver = 0;
    uint16_t sbs_null_cells = 0;
    uint8_t num_plp = 0;  // PLPs - 1
    uint8_t mimo_mixed = 0;
    std::vector<L1DetailPlp> plps;
};

struct L1Detail {
    uint8_t version = 0;
    uint8_t num_rf = 0;
    std::vector<uint16_t> bonded_bsid;
    uint32_t time_sec = 0;
    uint16_t time_msec = 0;
    uint16_t time_usec = 0;
    uint16_t time_nsec = 0;
    std::vector<L1DetailSubframe> subframes;
    uint16_t bsid = 0;
    uint32_t crc = 0;
};

// bits: kL1BasicBits. Returns false if the CRC does not match; out is
// filled either way.
bool parse_l1_basic(const uint8_t* bits, L1Basic* out);

// bits: 8 * basic.l1_detail_size_bytes. Returns false if the fields run
// past the end (before the CRC) or the CRC does not match.
bool parse_l1_detail(const uint8_t* bits, size_t num_bits, const L1Basic& basic, L1Detail* out);

}  // namespace framing
}  // namespace atsc3
