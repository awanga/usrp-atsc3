// atsc3_freq_deinterleaver.h — GNU Radio ATSC 3.0 Frequency De-interleaver Block
//
// AXI4-S: TDATA=int8_t TVALID TREADY TLAST(symbol)
// Input: Interleaved LLR cells (one OFDM symbol)
// Output: De-interleaved LLR cells

#ifndef INCLUDED_ATSC3_FREQ_DEINTERLEAVER_H
#define INCLUDED_ATSC3_FREQ_DEINTERLEAVER_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class freq_deinterleaver : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<freq_deinterleaver> sptr;

    /*!
     * \brief Create ATSC 3.0 frequency de-interleaver block
     * \param fft_size FFT size (8192, 16384, or 32768)
     * \return Shared pointer to new instance
     */
    static sptr make(int fft_size = 8192);

    /*!
     * \brief Set FFT size (reconfigures permutation)
     */
    virtual void set_fft_size(int fft_size) = 0;

    /*!
     * \brief Get current FFT size
     */
    virtual int get_fft_size() const = 0;

    /*!
     * \brief Get number of active carriers
     */
    virtual int get_num_carriers() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_FREQ_DEINTERLEAVER_H */
