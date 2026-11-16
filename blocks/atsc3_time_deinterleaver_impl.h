// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// atsc3_time_deinterleaver_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_TIME_DEINTERLEAVER_IMPL_H
#define INCLUDED_ATSC3_TIME_DEINTERLEAVER_IMPL_H

#include "atsc3_time_deinterleaver.h"

#include <memory>
#include <ofdm/time_deinterleaver.h>
#include <pmt/pmt.h>

namespace gr {
namespace atsc3 {

class time_deinterleaver_impl : public time_deinterleaver {
private:
    int ti_mode_;
    int ti_depth_;
    int cells_per_block_;
    int plp_id_;

    // CTI only: HTI needs TI Block boundaries a plain cell stream does not
    // carry, so reconfigure() rejects it.
    ::atsc3::ofdm::CtiDeinterleaver<int8_t> cti_;
    uint64_t cells_processed_ = 0;

    // Message ports
    pmt::pmt_t port_reset_in_;
    pmt::pmt_t port_l1_config_;

    void reconfigure();
    void handle_reset_msg(pmt::pmt_t msg);
    void handle_l1_config(pmt::pmt_t msg);

public:
    time_deinterleaver_impl(int ti_mode, int ti_depth);
    ~time_deinterleaver_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_ti_mode(int ti_mode) override;
    void set_ti_depth(int ti_depth) override;
    bool is_settled() const override {
        return ti_mode_ == 0 || cells_processed_ >= cti_.delay();
    }
    int get_settling_blocks() const override {
        if (ti_mode_ == 0) {
            return 0;
        }
        return static_cast<int>((cti_.delay() + cells_per_block_ - 1) / cells_per_block_);
    }
    void reset() override {
        reconfigure();
    }
    void set_plp_id(int plp_id) override;
    int get_plp_id() const override {
        return plp_id_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_TIME_DEINTERLEAVER_IMPL_H */
