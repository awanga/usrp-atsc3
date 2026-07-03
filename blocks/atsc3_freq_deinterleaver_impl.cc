// atsc3_freq_deinterleaver_impl.cc — Frequency De-interleaver implementation

#include "atsc3_freq_deinterleaver_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

freq_deinterleaver::sptr freq_deinterleaver::make(int fft_size) {
    return gnuradio::make_block_sptr<freq_deinterleaver_impl>(fft_size);
}

int freq_deinterleaver_impl::get_active_carriers(int fft_size) {
    switch (fft_size) {
        case 8192:
            return 6913;
        case 16384:
            return 13825;
        case 32768:
            return 27649;
        default:
            return 6913;
    }
}

freq_deinterleaver_impl::freq_deinterleaver_impl(int fft_size)
    : gr::sync_block("atsc3_freq_deinterleaver", gr::io_signature::make(1, 1, sizeof(int8_t)),
                     gr::io_signature::make(1, 1, sizeof(int8_t))),
      fft_size_(fft_size),
      num_carriers_(get_active_carriers(fft_size)) {
    reconfigure();
}

freq_deinterleaver_impl::~freq_deinterleaver_impl() = default;

void freq_deinterleaver_impl::reconfigure() {
    num_carriers_ = get_active_carriers(fft_size_);

    ::atsc3::ofdm::FreqDeinterleaverConfig config;
    config.fft_size = static_cast<size_t>(fft_size_);
    config.num_active_carriers = static_cast<size_t>(num_carriers_);

    deinterleaver_ = std::make_unique<::atsc3::ofdm::FreqDeinterleaver>(config);

    // Process one OFDM symbol at a time
    set_output_multiple(num_carriers_);
}

int freq_deinterleaver_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& output_items) {
    const int8_t* in = static_cast<const int8_t*>(input_items[0]);
    int8_t* out = static_cast<int8_t*>(output_items[0]);

    int symbols_to_process = noutput_items / num_carriers_;
    int produced = 0;

    for (int sym = 0; sym < symbols_to_process; ++sym) {
        const int8_t* sym_in = in + sym * num_carriers_;
        int8_t* sym_out = out + sym * num_carriers_;

        deinterleaver_->deinterleave(sym_in, sym_out);

        produced += num_carriers_;
    }

    return produced;
}

void freq_deinterleaver_impl::set_fft_size(int fft_size) {
    fft_size_ = fft_size;
    reconfigure();
}

}  // namespace atsc3
}  // namespace gr
