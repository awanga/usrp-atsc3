// atsc3_l1_monitor.h — GNU Radio ATSC 3.0 L1 Monitor Block
//
// Message-only block for monitoring L1 signaling
// Input: Message port "l1_config" (receives Atsc3Config updates)
// Output: Message port "l1_info" (JSON-formatted L1 info)

#ifndef INCLUDED_ATSC3_L1_MONITOR_H
#define INCLUDED_ATSC3_L1_MONITOR_H

#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class l1_monitor : virtual public gr::block {
public:
    typedef std::shared_ptr<l1_monitor> sptr;

    /*!
     * \brief Create ATSC 3.0 L1 monitor block
     * \return Shared pointer to new instance
     */
    static sptr make();

    /*!
     * \brief Get current FFT size
     */
    virtual int get_fft_size() const = 0;

    /*!
     * \brief Get current pilot pattern
     */
    virtual int get_pilot_pattern() const = 0;

    /*!
     * \brief Get number of PLPs
     */
    virtual int get_num_plps() const = 0;

    /*!
     * \brief Check if L1 config is valid
     */
    virtual bool is_config_valid() const = 0;

    /*!
     * \brief Get L1 info as JSON string
     */
    virtual std::string get_l1_json() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_L1_MONITOR_H */
