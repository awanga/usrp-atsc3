// atsc3_route_parser_impl.cc — ROUTE Parser implementation

#include "atsc3_route_parser_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

route_parser::sptr route_parser::make() {
    return gnuradio::make_block_sptr<route_parser_impl>();
}

route_parser_impl::route_parser_impl()
    : gr::sync_block("atsc3_route_parser", gr::io_signature::make(1, 1, sizeof(uint8_t)),
                     gr::io_signature::make(0, 0, 0)),
      port_catalog_(pmt::mp("catalog")),
      port_segment_(pmt::mp("segment")),
      packet_count_(0),
      error_count_(0) {
    // Register message ports
    message_port_register_out(port_catalog_);
    message_port_register_out(port_segment_);

    // Initialize catalog and parser
    catalog_ = std::make_unique<::atsc3::framing::ServiceCatalog>();
    parser_ = std::make_unique<::atsc3::framing::RouteParser>(catalog_.get());

    // Register segment callback
    parser_->set_segment_callback(
        [this](uint16_t service_id, const ::atsc3::framing::DashSegment& segment) {
            auto msg = build_segment_pmt(service_id, segment);
            message_port_pub(port_segment_, msg);
        });

    // Register catalog update callback
    catalog_->set_update_callback([this]() {
        auto msg = build_catalog_pmt();
        message_port_pub(port_catalog_, msg);
    });
}

route_parser_impl::~route_parser_impl() = default;

int route_parser_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                            gr_vector_void_star& /* output_items */) {
    const uint8_t* in = static_cast<const uint8_t*>(input_items[0]);

    // Process signaling data
    if (parser_->process_signaling(in, static_cast<size_t>(noutput_items))) {
        packet_count_++;
    } else {
        // Check if there were parse errors
        auto stats = parser_->get_stats();
        error_count_ = stats.parse_errors + stats.xml_errors;
    }

    return noutput_items;
}

size_t route_parser_impl::get_service_count() const {
    return catalog_->service_count();
}

void route_parser_impl::reset() {
    parser_->reset();
    catalog_->clear();
    packet_count_ = 0;
    error_count_ = 0;
}

pmt::pmt_t route_parser_impl::build_catalog_pmt() const {
    // Build PMT dictionary with service catalog info
    pmt::pmt_t dict = pmt::make_dict();

    dict = pmt::dict_add(dict, pmt::mp("service_count"),
                         pmt::from_long(static_cast<long>(catalog_->service_count())));

    // Add service list
    pmt::pmt_t services = pmt::make_vector(catalog_->service_count(), pmt::PMT_NIL);

    for (size_t i = 0; i < catalog_->service_count(); i++) {
        const auto* svc = catalog_->get_service(i);
        if (svc && svc->valid) {
            pmt::pmt_t svc_dict = pmt::make_dict();
            svc_dict =
                pmt::dict_add(svc_dict, pmt::mp("service_id"), pmt::from_long(svc->service_id));
            svc_dict = pmt::dict_add(svc_dict, pmt::mp("major_channel"),
                                     pmt::from_long(svc->major_channel));
            svc_dict = pmt::dict_add(svc_dict, pmt::mp("minor_channel"),
                                     pmt::from_long(svc->minor_channel));
            svc_dict = pmt::dict_add(svc_dict, pmt::mp("name"),
                                     pmt::intern(std::string(svc->short_name.data())));

            pmt::vector_set(services, i, svc_dict);
        }
    }

    dict = pmt::dict_add(dict, pmt::mp("services"), services);

    return dict;
}

pmt::pmt_t
route_parser_impl::build_segment_pmt(uint16_t service_id,
                                     const ::atsc3::framing::DashSegment& segment) const {
    pmt::pmt_t dict = pmt::make_dict();

    dict = pmt::dict_add(dict, pmt::mp("service_id"), pmt::from_long(service_id));
    dict = pmt::dict_add(dict, pmt::mp("toi"), pmt::from_long(segment.toi));
    dict = pmt::dict_add(dict, pmt::mp("url"), pmt::intern(segment.url));
    dict = pmt::dict_add(dict, pmt::mp("byte_offset"), pmt::from_uint64(segment.byte_offset));
    dict = pmt::dict_add(dict, pmt::mp("byte_length"), pmt::from_uint64(segment.byte_length));
    dict = pmt::dict_add(dict, pmt::mp("presentation_time"),
                         pmt::from_long(segment.presentation_time));
    dict = pmt::dict_add(dict, pmt::mp("duration"), pmt::from_long(segment.duration));

    return dict;
}

}  // namespace atsc3
}  // namespace gr
