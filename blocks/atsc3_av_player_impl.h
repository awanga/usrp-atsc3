// atsc3_av_player_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_AV_PLAYER_IMPL_H
#define INCLUDED_ATSC3_AV_PLAYER_IMPL_H

#include "atsc3_av_player.h"

#include <audio_decoder.h>
#include <gst_player.h>
#include <hevc_decoder.h>
#include <memory>
#include <mutex>

namespace gr {
namespace atsc3 {

class av_player_impl : public av_player {
private:
    bool enable_video_;
    bool enable_audio_;
    float volume_;

    std::unique_ptr<::atsc3::av::GstPlayer> player_;
    std::unique_ptr<::atsc3::av::HevcDecoder> video_decoder_;
    std::unique_ptr<::atsc3::av::AudioDecoder> audio_decoder_;

    // Message port for state changes
    pmt::pmt_t port_state_;

    // Statistics
    uint64_t video_frames_;
    uint64_t audio_samples_;

    mutable std::mutex mutex_;
    bool initialized_;

    // Convert internal state to PMT message
    pmt::pmt_t state_to_pmt(PlayerState state) const;

    // Handle state change callback from GstPlayer
    void on_state_change(::atsc3::av::PlayerState state);

public:
    av_player_impl(bool enable_video, bool enable_audio, float volume);
    ~av_player_impl() override;

    // GR lifecycle
    bool start() override;
    bool stop() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    bool play() override;
    bool pause() override;
    void set_volume(float volume) override;
    float get_volume() const override {
        return volume_;
    }
    PlayerState get_state() const override;
    uint64_t get_video_frames() const override {
        return video_frames_;
    }
    uint64_t get_audio_samples() const override {
        return audio_samples_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_AV_PLAYER_IMPL_H */
