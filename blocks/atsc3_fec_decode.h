// atsc3_fec_decode.h — GNU Radio ATSC 3.0 FEC Decoder Block
//
// AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
// Input: Soft LLR values from demapper
// Output: Decoded bits

#ifndef INCLUDED_ATSC3_FEC_DECODE_H
#define INCLUDED_ATSC3_FEC_DECODE_H

#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class fec_decode : virtual public gr::block {
public:
    typedef std::shared_ptr<fec_decode> sptr;

    /*!
     * \brief Create ATSC 3.0 FEC decoder block
     * \param code_rate LDPC code rate index (0-11)
     * \param codeword_length Codeword length (16200 or 64800)
     * \param max_iterations Maximum LDPC iterations
     * \return Shared pointer to new instance
     */
    static sptr make(int code_rate = 6, int codeword_length = 64800, int max_iterations = 50);

    /*!
     * \brief Set LDPC code rate
     */
    virtual void set_code_rate(int code_rate) = 0;

    /*!
     * \brief Set codeword length
     */
    virtual void set_codeword_length(int length) = 0;

    /*!
     * \brief Get average LDPC iterations
     */
    virtual float get_avg_iterations() const = 0;

    /*!
     * \brief Get frame error rate
     */
    virtual float get_fer() const = 0;

    /*!
     * \brief Check if last codeword converged
     */
    virtual bool last_converged() const = 0;

    /*!
     * \brief Reset decoder statistics
     *
     * Clears FER, average iterations, and codeword counters.
     * Called automatically via "reset" message port.
     */
    virtual void reset_stats() = 0;

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

#endif /* INCLUDED_ATSC3_FEC_DECODE_H */
