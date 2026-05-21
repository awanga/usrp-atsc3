// atsc3_alp_demux.h — GNU Radio ATSC 3.0 ALP Demultiplexer Block
//
// AXI4-S: TDATA=uint8 TVALID TREADY TLAST(packet) TID(plp_id)
// Input: Decoded baseband frames
// Output: ALP packets (IP or MPEG-TS encapsulated)

#ifndef INCLUDED_ATSC3_ALP_DEMUX_H
#define INCLUDED_ATSC3_ALP_DEMUX_H

#include <cstdint>
#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class alp_demux : virtual public gr::block {
public:
    typedef std::shared_ptr<alp_demux> sptr;

    /*!
     * \brief Create ATSC 3.0 ALP demultiplexer block
     * \param plp_id PLP ID to extract (0-63, or -1 for all)
     * \return Shared pointer to new instance
     */
    static sptr make(int plp_id = 0);

    /*!
     * \brief Set PLP ID to extract
     */
    virtual void set_plp_id(int plp_id) = 0;

    /*!
     * \brief Get current PLP ID
     */
    virtual int get_plp_id() const = 0;

    /*!
     * \brief Get packet count
     */
    virtual uint64_t get_packet_count() const = 0;

    /*!
     * \brief Get error count (CRC failures, etc.)
     */
    virtual uint64_t get_error_count() const = 0;

    /*!
     * \brief Get packet type distribution
     * Returns counts for: IP, MPEG-TS, signaling, other
     */
    virtual void get_packet_stats(uint64_t* ip_count, uint64_t* ts_count, uint64_t* sig_count,
                                  uint64_t* other_count) const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_ALP_DEMUX_H */
