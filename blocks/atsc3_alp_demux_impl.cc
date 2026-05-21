// atsc3_alp_demux_impl.cc — ALP Demultiplexer implementation

#include "atsc3_alp_demux_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

alp_demux::sptr alp_demux::make(int plp_id) {
    return gnuradio::make_block_sptr<alp_demux_impl>(plp_id);
}

alp_demux_impl::alp_demux_impl(int plp_id)
    : gr::block("atsc3_alp_demux", gr::io_signature::make(1, 1, sizeof(uint8_t)),
                gr::io_signature::make(1, 1, sizeof(uint8_t))),
      plp_id_(plp_id) {
    reconfigure();

    // Maximum ALP packet size is 65535 bytes
    packet_buffer_.reserve(65536);
}

alp_demux_impl::~alp_demux_impl() = default;

void alp_demux_impl::reconfigure() {
    // Configure ALP demultiplexer
    ::atsc3::framing::AlpDemuxConfig config;
    alp_demux_ = std::make_unique<::atsc3::framing::AlpDemux>(config);

    // Set up callbacks to collect packets
    alp_demux_->set_ip_callback([this](uint8_t plp, const uint8_t* data, size_t len) {
        if (plp_id_ < 0 || plp == static_cast<uint8_t>(plp_id_)) {
            // Store packet for output
            pending_packets_.emplace_back(data, data + len);
            pending_types_.push_back(PacketType::IP);
            ip_packet_count_++;
            packet_count_++;
        }
    });

    alp_demux_->set_signaling_callback([this](uint8_t plp, const uint8_t* data, size_t len) {
        if (plp_id_ < 0 || plp == static_cast<uint8_t>(plp_id_)) {
            pending_packets_.emplace_back(data, data + len);
            pending_types_.push_back(PacketType::SIGNALING);
            sig_packet_count_++;
            packet_count_++;
        }
    });

    alp_demux_->set_ts_callback([this](uint8_t plp, const uint8_t* data, size_t len) {
        if (plp_id_ < 0 || plp == static_cast<uint8_t>(plp_id_)) {
            pending_packets_.emplace_back(data, data + len);
            pending_types_.push_back(PacketType::TS);
            ts_packet_count_++;
            packet_count_++;
        }
    });
}

void alp_demux_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required) {
    // ALP parsing has variable expansion ratio
    // Conservatively request same amount of input as output
    ninput_items_required[0] = noutput_items;
}

int alp_demux_impl::general_work(int noutput_items, gr_vector_int& ninput_items,
                                 gr_vector_const_void_star& input_items,
                                 gr_vector_void_star& output_items) {
    const uint8_t* in = static_cast<const uint8_t*>(input_items[0]);
    uint8_t* out = static_cast<uint8_t*>(output_items[0]);

    int available = ninput_items[0];
    int produced = 0;

    // First, output any pending packets from previous calls
    while (!pending_packets_.empty() && produced < noutput_items) {
        auto& pkt = pending_packets_.front();
        if (produced + static_cast<int>(pkt.size()) > noutput_items) {
            break;  // Can't fit this packet
        }

        std::memcpy(out + produced, pkt.data(), pkt.size());

        // Add stream tag for packet boundary
        add_item_tag(0, nitems_written(0) + produced, pmt::intern("packet_len"),
                     pmt::from_long(pkt.size()));

        produced += static_cast<int>(pkt.size());
        pending_packets_.pop_front();
        pending_types_.pop_front();
    }

    // Process input data - callbacks will populate pending_packets_
    uint8_t plp_to_use = (plp_id_ >= 0) ? static_cast<uint8_t>(plp_id_) : 0;
    size_t packets_parsed = alp_demux_->process(plp_to_use, in, static_cast<size_t>(available));
    (void)packets_parsed;

    // Output newly received packets
    while (!pending_packets_.empty() && produced < noutput_items) {
        auto& pkt = pending_packets_.front();
        if (produced + static_cast<int>(pkt.size()) > noutput_items) {
            break;
        }

        std::memcpy(out + produced, pkt.data(), pkt.size());
        add_item_tag(0, nitems_written(0) + produced, pmt::intern("packet_len"),
                     pmt::from_long(pkt.size()));

        produced += static_cast<int>(pkt.size());
        pending_packets_.pop_front();
        pending_types_.pop_front();
    }

    // Check for errors
    const auto& stats = alp_demux_->get_stats();
    error_count_.store(stats.header_errors + stats.crc_errors + stats.reassembly_errors);

    consume_each(available);
    return produced;
}

void alp_demux_impl::set_plp_id(int plp_id) {
    plp_id_ = plp_id;
    // Note: callbacks filter by plp_id_, no need to reconfigure
}

void alp_demux_impl::get_packet_stats(uint64_t* ip_count, uint64_t* ts_count, uint64_t* sig_count,
                                      uint64_t* other_count) const {
    if (ip_count)
        *ip_count = ip_packet_count_.load();
    if (ts_count)
        *ts_count = ts_packet_count_.load();
    if (sig_count)
        *sig_count = sig_packet_count_.load();
    if (other_count)
        *other_count = other_packet_count_.load();
}

}  // namespace atsc3
}  // namespace gr
