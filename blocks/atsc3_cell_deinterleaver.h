// atsc3_cell_deinterleaver.h — GNU Radio ATSC 3.0 Cell De-interleaver Block
//
// AXI4-S: TDATA=int8_t TVALID TREADY TLAST(FEC block)
// Input: Interleaved LLR cells
// Output: De-interleaved LLR cells

#ifndef INCLUDED_ATSC3_CELL_DEINTERLEAVER_H
#define INCLUDED_ATSC3_CELL_DEINTERLEAVER_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class cell_deinterleaver : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<cell_deinterleaver> sptr;

    /*!
     * \brief Create ATSC 3.0 cell de-interleaver block
     * \param fft_size FFT size (determines number of cells)
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
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_CELL_DEINTERLEAVER_H */
