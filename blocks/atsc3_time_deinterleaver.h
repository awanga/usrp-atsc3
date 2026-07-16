// atsc3_time_deinterleaver.h — GNU Radio ATSC 3.0 Time De-interleaver Block
//
// AXI4-S: TDATA=int8_t TVALID TREADY TLAST(TI block)
// Input: Interleaved LLR cells
// Output: De-interleaved LLR cells

#ifndef INCLUDED_ATSC3_TIME_DEINTERLEAVER_H
#define INCLUDED_ATSC3_TIME_DEINTERLEAVER_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class time_deinterleaver : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<time_deinterleaver> sptr;

    /*!
     * \brief Create ATSC 3.0 time de-interleaver block
     * \param ti_mode Time interleaving mode (0=NONE, 1=CTI, 2=HTI)
     * \param ti_depth Time interleaving depth (0-15)
     * \return Shared pointer to new instance
     */
    static sptr make(int ti_mode = 1, int ti_depth = 0);

    /*!
     * \brief Set time interleaving mode
     */
    virtual void set_ti_mode(int ti_mode) = 0;

    /*!
     * \brief Set time interleaving depth
     */
    virtual void set_ti_depth(int ti_depth) = 0;

    /*!
     * \brief Check if delay lines are settled (output is valid)
     */
    virtual bool is_settled() const = 0;

    /*!
     * \brief Get number of blocks needed for settling
     */
    virtual int get_settling_blocks() const = 0;

    /*!
     * \brief Reset internal state (clears delay lines)
     */
    virtual void reset() = 0;

    /*!
     * \brief Set PLP ID for multi-PLP operation
     *
     * When L1 config is received, block auto-configures for this PLP.
     * \param plp_id PLP ID (0-63), or -1 to use manual configuration
     */
    virtual void set_plp_id(int plp_id) = 0;

    /*!
     * \brief Get current PLP ID
     */
    virtual int get_plp_id() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_TIME_DEINTERLEAVER_H */
