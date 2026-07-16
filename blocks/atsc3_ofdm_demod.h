// atsc3_ofdm_demod.h — GNU Radio ATSC 3.0 OFDM Demodulator Block
//
// AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
// Input: Time-domain IQ samples (CFO corrected)
// Output: Frequency-domain subcarriers per OFDM symbol

#ifndef INCLUDED_ATSC3_OFDM_DEMOD_H
#define INCLUDED_ATSC3_OFDM_DEMOD_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class ofdm_demod : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<ofdm_demod> sptr;

    /*!
     * \brief Create ATSC 3.0 OFDM demodulator block
     * \param fft_size FFT size (8192, 16384, or 32768)
     * \param cp_length Cyclic prefix length in samples
     * \return Shared pointer to new instance
     */
    static sptr make(int fft_size = 8192, int cp_length = 1024);

    /*!
     * \brief Set FFT size (reconfigurable via L1 signaling)
     */
    virtual void set_fft_size(int fft_size) = 0;

    /*!
     * \brief Set cyclic prefix length
     */
    virtual void set_cp_length(int cp_length) = 0;

    /*!
     * \brief Get current FFT size
     */
    virtual int get_fft_size() const = 0;

    /*!
     * \brief Get current CP length
     */
    virtual int get_cp_length() const = 0;

    /*!
     * \brief Check if frame sync is locked
     */
    virtual bool is_locked() const = 0;

    /*!
     * \brief Reset demodulator state
     *
     * Clears internal buffers and frame sync state.
     * Called automatically via "reset" message port.
     */
    virtual void reset() = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_OFDM_DEMOD_H */
