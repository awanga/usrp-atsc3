// atsc3_route_parser.h — GNU Radio ATSC 3.0 ROUTE Parser Block
//
// AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(packet)
// Input: Byte stream (signaling packets from ALP demux)
// Output: Message ports for service catalog and segment notifications

#ifndef INCLUDED_ATSC3_ROUTE_PARSER_H
#define INCLUDED_ATSC3_ROUTE_PARSER_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class route_parser : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<route_parser> sptr;

    /*!
     * \brief Create ATSC 3.0 ROUTE parser block
     * \return Shared pointer to new instance
     */
    static sptr make();

    /*!
     * \brief Get number of discovered services
     */
    virtual size_t get_service_count() const = 0;

    /*!
     * \brief Get parsed packet count
     */
    virtual uint64_t get_packet_count() const = 0;

    /*!
     * \brief Get parse error count
     */
    virtual uint64_t get_error_count() const = 0;

    /*!
     * \brief Reset parser state
     */
    virtual void reset() = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_ROUTE_PARSER_H */
