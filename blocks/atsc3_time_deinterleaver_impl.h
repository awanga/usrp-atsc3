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

    std::unique_ptr<::atsc3::ofdm::TimeDeinterleaver> deinterleaver_;

    // Message port
    pmt::pmt_t port_reset_in_;

    void reconfigure();
    void handle_reset_msg(pmt::pmt_t msg);

public:
    time_deinterleaver_impl(int ti_mode, int ti_depth);
    ~time_deinterleaver_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_ti_mode(int ti_mode) override;
    void set_ti_depth(int ti_depth) override;
    bool is_settled() const override {
        return deinterleaver_->is_settled();
    }
    int get_settling_blocks() const override {
        return static_cast<int>(deinterleaver_->get_settling_blocks());
    }
    void reset() override {
        deinterleaver_->reset();
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_TIME_DEINTERLEAVER_IMPL_H */
