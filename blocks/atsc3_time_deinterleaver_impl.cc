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
      cells_per_block_(1000),
      plp_id_(-1),
      port_reset_in_(pmt::intern("reset")),
      port_l1_config_(pmt::intern("l1_config")) {
    reconfigure();

    // Register message ports
    message_port_register_in(port_reset_in_);
    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });

    message_port_register_in(port_l1_config_);
    set_msg_handler(port_l1_config_, [this](pmt::pmt_t msg) { this->handle_l1_config(msg); });
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

void time_deinterleaver_impl::set_plp_id(int plp_id) {
    plp_id_ = plp_id;
}

void time_deinterleaver_impl::handle_l1_config(pmt::pmt_t msg) {
    // Skip auto-configuration if manual mode (plp_id_ == -1)
    if (plp_id_ < 0) {
        return;
    }

    if (!pmt::is_dict(msg)) {
        return;
    }

    pmt::pmt_t plps_key = pmt::intern("plps");
    if (!pmt::dict_has_key(msg, plps_key)) {
        return;
    }

    pmt::pmt_t plps = pmt::dict_ref(msg, plps_key, pmt::PMT_NIL);
    if (!pmt::is_vector(plps)) {
        return;
    }

    // Search for our PLP ID in the configuration
    size_t num_plps = pmt::length(plps);
    for (size_t i = 0; i < num_plps; ++i) {
        pmt::pmt_t plp_config = pmt::vector_ref(plps, i);
        if (!pmt::is_dict(plp_config)) {
            continue;
        }

        pmt::pmt_t plp_id_key = pmt::intern("plp_id");
        if (!pmt::dict_has_key(plp_config, plp_id_key)) {
            continue;
        }

        int config_plp_id = pmt::to_long(pmt::dict_ref(plp_config, plp_id_key, pmt::from_long(-1)));
        if (config_plp_id != plp_id_) {
            continue;
        }

        // Found our PLP - extract TI mode and depth
        pmt::pmt_t ti_mode_key = pmt::intern("ti_mode");
        pmt::pmt_t ti_depth_key = pmt::intern("ti_depth");

        bool need_reconfigure = false;

        if (pmt::dict_has_key(plp_config, ti_mode_key)) {
            int new_mode =
                pmt::to_long(pmt::dict_ref(plp_config, ti_mode_key, pmt::from_long(ti_mode_)));
            if (new_mode != ti_mode_) {
                ti_mode_ = new_mode;
                need_reconfigure = true;
            }
        }

        if (pmt::dict_has_key(plp_config, ti_depth_key)) {
            int new_depth =
                pmt::to_long(pmt::dict_ref(plp_config, ti_depth_key, pmt::from_long(ti_depth_)));
            if (new_depth != ti_depth_) {
                ti_depth_ = new_depth;
                need_reconfigure = true;
            }
        }

        if (need_reconfigure) {
            reconfigure();
        }
        break;
    }
}

}  // namespace atsc3
}  // namespace gr
