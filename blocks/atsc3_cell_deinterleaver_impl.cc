// atsc3_cell_deinterleaver_impl.cc — Cell De-interleaver implementation

#include "atsc3_cell_deinterleaver_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

cell_deinterleaver::sptr cell_deinterleaver::make(int fft_size) {
    return gnuradio::make_block_sptr<cell_deinterleaver_impl>(fft_size);
}

cell_deinterleaver_impl::cell_deinterleaver_impl(int fft_size)
    : gr::sync_block("atsc3_cell_deinterleaver", gr::io_signature::make(1, 1, sizeof(int8_t)),
                     gr::io_signature::make(1, 1, sizeof(int8_t))),
      fft_size_(fft_size) {
    reconfigure();
}

cell_deinterleaver_impl::~cell_deinterleaver_impl() = default;

void cell_deinterleaver_impl::reconfigure() {
    ::atsc3::ofdm::CellDeinterleaverConfig config;
    config.num_cells = static_cast<size_t>(fft_size_);

    deinterleaver_ = std::make_unique<::atsc3::ofdm::CellDeinterleaver>(config);

    // Process one FEC block at a time
    set_output_multiple(fft_size_);
}

int cell_deinterleaver_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& output_items) {
    const int8_t* in = static_cast<const int8_t*>(input_items[0]);
    int8_t* out = static_cast<int8_t*>(output_items[0]);

    int blocks_to_process = noutput_items / fft_size_;
    int produced = 0;

    for (int blk = 0; blk < blocks_to_process; ++blk) {
        const int8_t* blk_in = in + blk * fft_size_;
        int8_t* blk_out = out + blk * fft_size_;

        deinterleaver_->deinterleave(blk_in, blk_out, static_cast<size_t>(fft_size_));

        produced += fft_size_;
    }

    return produced;
}

void cell_deinterleaver_impl::set_fft_size(int fft_size) {
    fft_size_ = fft_size;
    reconfigure();
}

}  // namespace atsc3
}  // namespace gr
