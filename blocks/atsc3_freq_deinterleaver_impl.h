// atsc3_freq_deinterleaver_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_FREQ_DEINTERLEAVER_IMPL_H
#define INCLUDED_ATSC3_FREQ_DEINTERLEAVER_IMPL_H

#include "atsc3_freq_deinterleaver.h"

#include <memory>
#include <ofdm/freq_deinterleaver.h>

namespace gr {
namespace atsc3 {

class freq_deinterleaver_impl : public freq_deinterleaver {
private:
    int fft_size_;
    int num_carriers_;

    std::unique_ptr<::atsc3::ofdm::FreqDeinterleaver> deinterleaver_;

    void reconfigure();

    // Get active carriers for FFT size
    static int get_active_carriers(int fft_size);

public:
    explicit freq_deinterleaver_impl(int fft_size);
    ~freq_deinterleaver_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_fft_size(int fft_size) override;
    int get_fft_size() const override {
        return fft_size_;
    }
    int get_num_carriers() const override {
        return num_carriers_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_FREQ_DEINTERLEAVER_IMPL_H */
