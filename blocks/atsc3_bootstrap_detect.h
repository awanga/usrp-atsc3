// atsc3_bootstrap_detect.h — GNU Radio ATSC 3.0 Bootstrap Detector Block
//
// AXI4-S: TDATA=cf32 TVALID TREADY TLAST(frame)
// Input: IQ samples at 6.144 MS/s
// Output: IQ samples with bootstrap timing tag, coarse CFO estimate

#ifndef INCLUDED_ATSC3_BOOTSTRAP_DETECT_H
#define INCLUDED_ATSC3_BOOTSTRAP_DETECT_H

#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class bootstrap_detect : virtual public gr::block {
public:
    typedef std::shared_ptr<bootstrap_detect> sptr;

    /*!
     * \brief Create ATSC 3.0 bootstrap detector block
     * \param threshold Detection threshold (default 0.7)
     * \return Shared pointer to new instance
     */
    static sptr make(float threshold = 0.7f);

    /*!
     * \brief Get current coarse CFO estimate in Hz
     */
    virtual float get_cfo_hz() const = 0;

    /*!
     * \brief Check if bootstrap is currently locked
     */
    virtual bool is_locked() const = 0;

    /*!
     * \brief Set detection threshold
     */
    virtual void set_threshold(float threshold) = 0;

    /*!
     * \brief Reset detector state
     *
     * Clears internal buffers and returns to searching mode.
     * Called automatically via "reset" message port.
     */
    virtual void reset() = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_BOOTSTRAP_DETECT_H */
