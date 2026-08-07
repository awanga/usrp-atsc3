// atsc3_service_selector.h — GNU Radio ATSC 3.0 Service Selector Block
//
// AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(packet)
// Input: IP packets from ALP demux
// Output 0: Video ES (HEVC NALs)
// Output 1: Audio ES (AC-4/AAC frames)

#ifndef INCLUDED_ATSC3_SERVICE_SELECTOR_H
#define INCLUDED_ATSC3_SERVICE_SELECTOR_H

#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class service_selector : virtual public gr::block {
public:
    typedef std::shared_ptr<service_selector> sptr;

    /*!
     * \brief Create ATSC 3.0 service selector block
     * \param service_id Service ID to select (0 = first available)
     * \return Shared pointer to new instance
     */
    static sptr make(int service_id = 0);

    /*!
     * \brief Set service to extract
     * \param service_id Service ID to select
     */
    virtual void set_service_id(int service_id) = 0;

    /*!
     * \brief Get currently selected service ID
     */
    virtual int get_service_id() const = 0;

    /*!
     * \brief Get video bytes extracted
     */
    virtual uint64_t get_video_bytes() const = 0;

    /*!
     * \brief Get audio bytes extracted
     */
    virtual uint64_t get_audio_bytes() const = 0;

    /*!
     * \brief Check if service is locked
     */
    virtual bool is_service_locked() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_SERVICE_SELECTOR_H */
