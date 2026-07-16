// atsc3_channel_eq.h — GNU Radio ATSC 3.0 Channel Equalizer Block
//
// AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
// Input: Frequency-domain OFDM symbols
// Output: Equalized QAM symbols

#ifndef INCLUDED_ATSC3_CHANNEL_EQ_H
#define INCLUDED_ATSC3_CHANNEL_EQ_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class channel_eq : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<channel_eq> sptr;

    /*!
     * \brief Create ATSC 3.0 channel equalizer block
     * \param fft_size FFT size
     * \param pilot_pattern Pilot pattern (1-8)
     * \param use_mmse Use MMSE equalization (vs ZF)
     * \return Shared pointer to new instance
     */
    static sptr make(int fft_size = 8192, int pilot_pattern = 3, bool use_mmse = true);

    /*!
     * \brief Set pilot pattern
     */
    virtual void set_pilot_pattern(int pattern) = 0;

    /*!
     * \brief Set equalization mode
     */
    virtual void set_mmse(bool use_mmse) = 0;

    /*!
     * \brief Get current SNR estimate in dB
     */
    virtual float get_snr_db() const = 0;

    /*!
     * \brief Get current MER estimate in dB
     */
    virtual float get_mer_db() const = 0;

    /*!
     * \brief Reset equalizer state
     *
     * Clears channel estimate and IIR averaging state.
     * Called automatically via "reset" message port.
     */
    virtual void reset() = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_CHANNEL_EQ_H */
