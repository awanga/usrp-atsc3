// atsc3_channel_eq_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_CHANNEL_EQ_IMPL_H
#define INCLUDED_ATSC3_CHANNEL_EQ_IMPL_H

#include "atsc3_channel_eq.h"

#include <channel/channel_estimator.h>
#include <channel/equalizer.h>
#include <memory>
#include <ofdm/pilot_extractor.h>

namespace gr {
namespace atsc3 {

class channel_eq_impl : public channel_eq {
private:
    int fft_size_;
    int pilot_pattern_;
    bool use_mmse_;

    std::unique_ptr<::atsc3::ofdm::PilotExtractor> pilot_extractor_;
    std::unique_ptr<::atsc3::channel::ChannelEstimator> channel_estimator_;
    std::unique_ptr<::atsc3::channel::Equalizer> equalizer_;

    float snr_db_;
    float mer_db_;
    size_t symbol_index_;

    void reconfigure();

public:
    channel_eq_impl(int fft_size, int pilot_pattern, bool use_mmse);
    ~channel_eq_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_pilot_pattern(int pattern) override;
    void set_mmse(bool use_mmse) override;
    float get_snr_db() const override {
        return snr_db_;
    }
    float get_mer_db() const override {
        return mer_db_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_CHANNEL_EQ_IMPL_H */
