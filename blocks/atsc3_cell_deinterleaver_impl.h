// atsc3_cell_deinterleaver_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_CELL_DEINTERLEAVER_IMPL_H
#define INCLUDED_ATSC3_CELL_DEINTERLEAVER_IMPL_H

#include "atsc3_cell_deinterleaver.h"

#include <memory>
#include <ofdm/cell_deinterleaver.h>

namespace gr {
namespace atsc3 {

class cell_deinterleaver_impl : public cell_deinterleaver {
private:
    int fft_size_;

    std::unique_ptr<::atsc3::ofdm::CellDeinterleaver> deinterleaver_;

    void reconfigure();

public:
    explicit cell_deinterleaver_impl(int fft_size);
    ~cell_deinterleaver_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_fft_size(int fft_size) override;
    int get_fft_size() const override {
        return fft_size_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_CELL_DEINTERLEAVER_IMPL_H */
