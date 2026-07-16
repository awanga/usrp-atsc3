// atsc3_bootstrap_detect_impl.cc — Bootstrap Detector implementation

#include "atsc3_bootstrap_detect_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

namespace {
::atsc3::sync::BootstrapConfig make_detector_config(float threshold) {
    ::atsc3::sync::BootstrapConfig config;
    config.threshold = static_cast<double>(threshold);
    return config;
}
}  // namespace

bootstrap_detect::sptr bootstrap_detect::make(float threshold) {
    return gnuradio::make_block_sptr<bootstrap_detect_impl>(threshold);
}

bootstrap_detect_impl::bootstrap_detect_impl(float threshold)
    : gr::block("atsc3_bootstrap_detect", gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(1, 1, sizeof(gr_complex))),
      detector_(make_detector_config(threshold)),
      threshold_(threshold),
      locked_(false),
      prev_locked_(false),
      cfo_hz_(0.0f),
      metric_(0.0f) {
    // Set output multiple to bootstrap symbol length
    set_output_multiple(4096);

    // Register message ports
    port_reset_in_ = pmt::intern("reset");
    port_lock_status_out_ = pmt::intern("lock_status");

    message_port_register_in(port_reset_in_);
    message_port_register_out(port_lock_status_out_);

    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });
}

bootstrap_detect_impl::~bootstrap_detect_impl() = default;

void bootstrap_detect_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required) {
    // Need at least 2x bootstrap length for correlation
    ninput_items_required[0] = noutput_items + 8192;
}

int bootstrap_detect_impl::general_work(int noutput_items, gr_vector_int& ninput_items,
                                        gr_vector_const_void_star& input_items,
                                        gr_vector_void_star& output_items) {
    const gr_complex* in = static_cast<const gr_complex*>(input_items[0]);
    gr_complex* out = static_cast<gr_complex*>(output_items[0]);

    int ninput = ninput_items[0];
    int consumed = 0;
    int produced = 0;

    // Process samples through detector
    auto result = detector_.process(reinterpret_cast<const ::atsc3::ATSC3_SAMPLE_T*>(in),
                                    static_cast<size_t>(ninput));

    if (result.detected) {
        locked_ = true;
        cfo_hz_ = static_cast<float>(result.cfo_hz);
        metric_ = static_cast<float>(result.metric);

        // Add stream tag at detection point
        add_item_tag(0, nitems_written(0) + result.sample_index, pmt::intern("bootstrap_start"),
                     pmt::from_long(result.sample_index));
        add_item_tag(0, nitems_written(0) + result.sample_index, pmt::intern("cfo_hz"),
                     pmt::from_double(result.cfo_hz));
    }

    // Emit lock status message on state change
    if (locked_ != prev_locked_) {
        emit_lock_status();
        prev_locked_ = locked_;
    }

    // Pass through samples (detector is observe-only)
    int to_copy = std::min(ninput, noutput_items);
    std::memcpy(out, in, to_copy * sizeof(gr_complex));
    consumed = to_copy;
    produced = to_copy;

    consume_each(consumed);
    return produced;
}

void bootstrap_detect_impl::set_threshold(float threshold) {
    threshold_ = threshold;
    ::atsc3::sync::BootstrapConfig config;
    config.threshold = static_cast<double>(threshold);
    detector_.set_config(config);
}

void bootstrap_detect_impl::reset() {
    detector_.reset();
    locked_ = false;
    prev_locked_ = false;
    cfo_hz_ = 0.0f;
    metric_ = 0.0f;
}

void bootstrap_detect_impl::handle_reset_msg(pmt::pmt_t /*msg*/) {
    reset();
}

void bootstrap_detect_impl::emit_lock_status() {
    pmt::pmt_t msg = pmt::make_dict();
    msg = pmt::dict_add(msg, pmt::intern("locked"), pmt::from_bool(locked_));
    msg = pmt::dict_add(msg, pmt::intern("cfo_hz"), pmt::from_float(cfo_hz_));
    msg = pmt::dict_add(msg, pmt::intern("metric"), pmt::from_float(metric_));
    message_port_pub(port_lock_status_out_, msg);
}

}  // namespace atsc3
}  // namespace gr
