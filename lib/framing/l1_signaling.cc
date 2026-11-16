// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// l1_signaling.cc — A/322 L1-Basic and L1-Detail field parsing
//
// Reference: ATSC A/322 Section 9, Tables 9.2 and 9.9

#include "l1_signaling.h"

#include "fec/bch_decoder.h"

namespace atsc3 {
namespace framing {

namespace {

class BitReader {
public:
    BitReader(const uint8_t* bits, size_t num_bits) : bits_(bits), end_(num_bits) {}

    uint32_t get(size_t n) {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++) {
            v <<= 1;
            if (pos_ < end_) {
                v |= bits_[pos_] & 1u;
            }
            pos_++;
        }
        return v;
    }

    template <typename T>
    void read(T* field, size_t n) {
        *field = static_cast<T>(get(n));
    }

    bool overrun() const {
        return pos_ > end_;
    }
    size_t pos() const {
        return pos_;
    }

private:
    const uint8_t* bits_;
    size_t end_;
    size_t pos_ = 0;
};

uint32_t trailing_crc(const uint8_t* bits, size_t num_bits) {
    uint32_t v = 0;
    for (size_t i = num_bits - 32; i < num_bits; i++) {
        v = (v << 1) | (bits[i] & 1u);
    }
    return v;
}

}  // namespace

bool parse_l1_basic(const uint8_t* bits, L1Basic* b) {
    *b = L1Basic{};
    BitReader r(bits, kL1BasicBits);
    r.read(&b->version, 3);
    r.read(&b->mimo_scattered_pilot_encoding, 1);
    r.read(&b->lls_flag, 1);
    r.read(&b->time_info_flag, 2);
    r.read(&b->return_channel_flag, 1);
    r.read(&b->papr_reduction, 2);
    r.read(&b->frame_length_mode, 1);
    if (b->frame_length_mode == 0) {
        r.read(&b->frame_length, 10);
        r.read(&b->excess_samples_per_symbol, 13);
    } else {
        r.read(&b->time_offset, 16);
        r.read(&b->additional_samples, 7);
    }
    r.read(&b->num_subframes, 8);
    r.read(&b->preamble_num_symbols, 3);
    r.read(&b->preamble_reduced_carriers, 3);
    r.read(&b->l1_detail_content_tag, 2);
    r.read(&b->l1_detail_size_bytes, 13);
    r.read(&b->l1_detail_fec_type, 3);
    r.read(&b->l1_detail_additional_parity_mode, 2);
    r.read(&b->l1_detail_total_cells, 19);
    r.read(&b->first_sub_mimo, 1);
    r.read(&b->first_sub_miso, 2);
    r.read(&b->first_sub_fft_size, 2);
    r.read(&b->first_sub_reduced_carriers, 3);
    r.read(&b->first_sub_guard_interval, 4);
    r.read(&b->first_sub_num_ofdm_symbols, 11);
    r.read(&b->first_sub_scattered_pilot_pattern, 5);
    r.read(&b->first_sub_scattered_pilot_boost, 3);
    r.read(&b->first_sub_sbs_first, 1);
    r.read(&b->first_sub_sbs_last, 1);
    r.read(&b->first_sub_mimo_mixed, 1);
    b->crc = trailing_crc(bits, kL1BasicBits);
    return fec::outer_crc32(bits, kL1BasicBits - 32) == b->crc;
}

bool parse_l1_detail(const uint8_t* bits, size_t num_bits, const L1Basic& basic, L1Detail* d) {
    *d = L1Detail{};
    if (num_bits < 32 + 4) {
        return false;
    }
    const size_t body = num_bits - 32;
    BitReader r(bits, body);
    r.read(&d->version, 4);
    r.read(&d->num_rf, 3);
    for (size_t k = 0; k < d->num_rf; k++) {
        d->bonded_bsid.push_back(static_cast<uint16_t>(r.get(16)));
        r.get(3);  // reserved
    }
    if (basic.time_info_flag != 0) {
        r.read(&d->time_sec, 32);
        r.read(&d->time_msec, 10);
        if (basic.time_info_flag != 1) {
            r.read(&d->time_usec, 10);
            if (basic.time_info_flag != 2) {
                r.read(&d->time_nsec, 10);
            }
        }
    }
    d->subframes.resize(static_cast<size_t>(basic.num_subframes) + 1);
    for (size_t i = 0; i < d->subframes.size(); i++) {
        L1DetailSubframe& sf = d->subframes[i];
        if (i == 0) {
            sf.mimo = basic.first_sub_mimo;
            sf.miso = basic.first_sub_miso;
            sf.fft_size = basic.first_sub_fft_size;
            sf.reduced_carriers = basic.first_sub_reduced_carriers;
            sf.guard_interval = basic.first_sub_guard_interval;
            sf.num_ofdm_symbols = basic.first_sub_num_ofdm_symbols;
            sf.scattered_pilot_pattern = basic.first_sub_scattered_pilot_pattern;
            sf.scattered_pilot_boost = basic.first_sub_scattered_pilot_boost;
            sf.sbs_first = basic.first_sub_sbs_first;
            sf.sbs_last = basic.first_sub_sbs_last;
            sf.mimo_mixed = basic.first_sub_mimo_mixed;
        } else {
            r.read(&sf.mimo, 1);
            r.read(&sf.miso, 2);
            r.read(&sf.fft_size, 2);
            r.read(&sf.reduced_carriers, 3);
            r.read(&sf.guard_interval, 4);
            r.read(&sf.num_ofdm_symbols, 11);
            r.read(&sf.scattered_pilot_pattern, 5);
            r.read(&sf.scattered_pilot_boost, 3);
            r.read(&sf.sbs_first, 1);
            r.read(&sf.sbs_last, 1);
        }
        if (basic.num_subframes > 0) {
            r.read(&sf.subframe_multiplex, 1);
        }
        r.read(&sf.frequency_interleaver, 1);
        if (sf.sbs_first || sf.sbs_last) {
            r.read(&sf.sbs_null_cells, 13);
        }
        r.read(&sf.num_plp, 6);
        sf.plps.resize(static_cast<size_t>(sf.num_plp) + 1);
        for (L1DetailPlp& p : sf.plps) {
            r.read(&p.id, 6);
            r.read(&p.lls_flag, 1);
            r.read(&p.layer, 2);
            r.read(&p.start, 24);
            r.read(&p.size, 24);
            r.read(&p.scrambler_type, 2);
            r.read(&p.fec_type, 4);
            if (p.fec_type <= 5) {
                r.read(&p.mod, 4);
                r.read(&p.cod, 4);
            }
            r.read(&p.ti_mode, 2);
            if (p.ti_mode == 0) {
                r.read(&p.fec_block_start, 15);
            } else if (p.ti_mode == 1) {
                r.read(&p.cti_fec_block_start, 22);
            }
            if (d->num_rf > 0) {
                r.read(&p.num_channel_bonded, 3);
                if (p.num_channel_bonded > 0) {
                    r.read(&p.channel_bonding_format, 2);
                    for (size_t k = 0; k <= p.num_channel_bonded; k++) {
                        p.bonded_rf_id.push_back(static_cast<uint8_t>(r.get(3)));
                    }
                }
            }
            if (sf.mimo) {
                r.read(&p.mimo_stream_combining, 1);
                r.read(&p.mimo_iq_interleaving, 1);
                r.read(&p.mimo_ph, 1);
            }
            if (p.layer == 0) {
                r.read(&p.type, 1);
                if (p.type == 1) {
                    r.read(&p.num_subslices, 14);
                    r.read(&p.subslice_interval, 24);
                }
                if ((p.ti_mode == 1 || p.ti_mode == 2) && p.mod == 0) {
                    r.read(&p.ti_extended_interleaving, 1);
                }
                if (p.ti_mode == 1) {
                    r.read(&p.cti_depth, 3);
                    r.read(&p.cti_start_row, 11);
                } else if (p.ti_mode == 2) {
                    r.read(&p.hti_inter_subframe, 1);
                    r.read(&p.hti_num_ti_blocks, 4);
                    r.read(&p.hti_num_fec_blocks_max, 12);
                    size_t count = p.hti_inter_subframe ? p.hti_num_ti_blocks + 1u : 1u;
                    for (size_t k = 0; k < count; k++) {
                        p.hti_num_fec_blocks.push_back(static_cast<uint16_t>(r.get(12)));
                    }
                    r.read(&p.hti_cell_interleaver, 1);
                }
            } else {
                r.read(&p.ldm_injection_level, 5);
            }
        }
    }
    r.read(&d->bsid, 16);
    if (d->version >= 2) {
        for (size_t i = 0; i < d->subframes.size(); i++) {
            L1DetailSubframe& sf = d->subframes[i];
            if (i > 0) {
                r.read(&sf.mimo_mixed, 1);
            }
            if (!sf.mimo_mixed) {
                continue;
            }
            for (L1DetailPlp& p : sf.plps) {
                r.read(&p.mimo, 1);
                if (p.mimo) {
                    r.read(&p.mimo_stream_combining, 1);
                    r.read(&p.mimo_iq_interleaving, 1);
                    r.read(&p.mimo_ph, 1);
                }
            }
        }
    }
    d->crc = trailing_crc(bits, num_bits);
    if (r.overrun()) {
        return false;
    }
    return fec::outer_crc32(bits, body) == d->crc;
}

}  // namespace framing
}  // namespace atsc3
