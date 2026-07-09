// atsc3_route_parser_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_ROUTE_PARSER_IMPL_H
#define INCLUDED_ATSC3_ROUTE_PARSER_IMPL_H

#include "atsc3_route_parser.h"

#include <framing/route_parser.h>
#include <framing/service_catalog.h>
#include <memory>

namespace gr {
namespace atsc3 {

class route_parser_impl : public route_parser {
private:
    std::unique_ptr<::atsc3::framing::ServiceCatalog> catalog_;
    std::unique_ptr<::atsc3::framing::RouteParser> parser_;

    // Message ports
    pmt::pmt_t port_catalog_;
    pmt::pmt_t port_segment_;

    // Statistics
    uint64_t packet_count_;
    uint64_t error_count_;

    // Build catalog PMT from service list
    pmt::pmt_t build_catalog_pmt() const;

    // Build segment PMT
    pmt::pmt_t build_segment_pmt(uint16_t service_id,
                                 const ::atsc3::framing::DashSegment& segment) const;

public:
    route_parser_impl();
    ~route_parser_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    size_t get_service_count() const override;
    uint64_t get_packet_count() const override {
        return packet_count_;
    }
    uint64_t get_error_count() const override {
        return error_count_;
    }
    void reset() override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_ROUTE_PARSER_IMPL_H */
