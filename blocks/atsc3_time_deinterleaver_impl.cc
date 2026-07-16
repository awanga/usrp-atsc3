// atsc3_time_deinterleaver_impl.cc — Time De-interleaver implementation

#include "atsc3_time_deinterleaver_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

time_deinterleaver::sptr time_deinterleaver::make(int ti_mode, int ti_depth) {
    return gnuradio::make_block_sptr<time_deinterleaver_impl>(ti_mode, ti_depth);
}

time_deinterleaver_impl::time_deinterleaver_impl(int ti_mode, int ti_depth)
    : gr::sync_block("atsc3_time_deinterleaver", gr::io_signature::make(1, 1, sizeof(int8_t)),
                     gr::io_signature::make(1, 1, sizeof(int8_t))),
      ti_mode_(ti_mode),
      ti_depth_(ti_depth),
      cells_per_block_(1000) {  // Default cells per TI block
    reconfigure();

    // Register message port
    port_reset_in_ = pmt::intern("reset");
    message_port_register_in(port_reset_in_);
    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });
}

time_deinterleaver_impl::~time_deinterleaver_impl() = default;

void time_deinterleaver_impl::reconfigure() {
    ::atsc3::ofdm::TimeDeinterleaverConfig config;
    config.mode = static_cast<::atsc3::config::TimeInterleaveMode>(ti_mode_);
    config.depth = static_cast<uint8_t>(ti_depth_);
    config.cells_per_block = static_cast<size_t>(cells_per_block_);
    config.num_ti_blocks = 0;  // Not used for CTI

    deinterleaver_ = std::make_unique<::atsc3::ofdm::TimeDeinterleaver>(config);

    // Process one TI block at a time
    set_output_multiple(cells_per_block_);
}

int time_deinterleaver_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& output_items) {
    const int8_t* in = static_cast<const int8_t*>(input_items[0]);
    int8_t* out = static_cast<int8_t*>(output_items[0]);

    int blocks_to_process = noutput_items / cells_per_block_;
    int produced = 0;

    for (int blk = 0; blk < blocks_to_process; ++blk) {
        const int8_t* blk_in = in + blk * cells_per_block_;
        int8_t* blk_out = out + blk * cells_per_block_;

        deinterleaver_->process(blk_in, blk_out, static_cast<size_t>(cells_per_block_));

        produced += cells_per_block_;
    }

    return produced;
}

void time_deinterleaver_impl::set_ti_mode(int ti_mode) {
    ti_mode_ = ti_mode;
    reconfigure();
}

void time_deinterleaver_impl::set_ti_depth(int ti_depth) {
    ti_depth_ = ti_depth;
    reconfigure();
}

void time_deinterleaver_impl::handle_reset_msg(pmt::pmt_t /*msg*/) {
    reset();
}

}  // namespace atsc3
}  // namespace gr
