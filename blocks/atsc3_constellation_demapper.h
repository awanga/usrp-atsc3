// atsc3_constellation_demapper.h — GNU Radio ATSC 3.0 Constellation Demapper Block
//
// AXI4-S: TDATA=cf32 in, int8_t LLR out TVALID TREADY TLAST(codeword)
// Input: Equalized QAM symbols (gr_complex)
// Output: Soft LLRs (int8_t, bits_per_symbol per input symbol)

#ifndef INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_H
#define INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_H

#include <gnuradio/sync_interpolator.h>

namespace gr {
namespace atsc3 {

class constellation_demapper : virtual public gr::sync_interpolator {
public:
    typedef std::shared_ptr<constellation_demapper> sptr;

    /*!
     * \brief Create ATSC 3.0 constellation demapper block
     * \param modulation Modulation type (0=QPSK, 1=QAM16, 2=QAM64, etc.)
     * \param code_rate Code rate index (0=2/15, 1=3/15, ..., 11=13/15)
     * \param noise_variance Noise variance for LLR scaling
     * \return Shared pointer to new instance
     */
    static sptr make(int modulation = 2, int code_rate = 5, float noise_variance = 0.1f);

    /*!
     * \brief Set modulation type
     */
    virtual void set_modulation(int modulation) = 0;

    /*!
     * \brief Set code rate
     */
    virtual void set_code_rate(int code_rate) = 0;

    /*!
     * \brief Set noise variance for LLR scaling
     */
    virtual void set_noise_variance(float noise_variance) = 0;

    /*!
     * \brief Get bits per symbol for current modulation
     */
    virtual int get_bits_per_symbol() const = 0;

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

#endif /* INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_H */
