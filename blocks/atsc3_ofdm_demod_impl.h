// atsc3_ofdm_demod_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_OFDM_DEMOD_IMPL_H
#define INCLUDED_ATSC3_OFDM_DEMOD_IMPL_H

#include "atsc3_ofdm_demod.h"

#include <memory>
#include <ofdm/cp_removal.h>
#include <ofdm/fft_engine.h>
#include <vector>

namespace gr {
namespace atsc3 {

class ofdm_demod_impl : public ofdm_demod {
private:
    int fft_size_;
    int cp_length_;

    std::unique_ptr<::atsc3::ofdm::CpRemoval> cp_removal_;
    std::unique_ptr<::atsc3::ofdm::FftEngine> fft_engine_;

    std::vector<::atsc3::sample_t> fft_input_;
    std::vector<::atsc3::sample_t> fft_output_;
    bool symbol_ready_ = false;

    void reconfigure();

public:
    ofdm_demod_impl(int fft_size, int cp_length);
    ~ofdm_demod_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_fft_size(int fft_size) override;
    void set_cp_length(int cp_length) override;
    int get_fft_size() const override {
        return fft_size_;
    }
    int get_cp_length() const override {
        return cp_length_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_OFDM_DEMOD_IMPL_H */
