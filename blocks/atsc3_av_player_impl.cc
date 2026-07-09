// atsc3_av_player_impl.cc — A/V Player implementation

#include "atsc3_av_player_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

av_player::sptr av_player::make(bool enable_video, bool enable_audio, float volume) {
    return gnuradio::make_block_sptr<av_player_impl>(enable_video, enable_audio, volume);
}

av_player_impl::av_player_impl(bool enable_video, bool enable_audio, float volume)
    : gr::sync_block("atsc3_av_player", gr::io_signature::make(2, 2, sizeof(uint8_t)),
                     gr::io_signature::make(0, 0, 0)),
      enable_video_(enable_video),
      enable_audio_(enable_audio),
      volume_(volume),
      video_frames_(0),
      audio_samples_(0),
      initialized_(false) {
    // Create message port for state changes
    port_state_ = pmt::mp("state");
    message_port_register_out(port_state_);
}

av_player_impl::~av_player_impl() {
    if (player_) {
        player_->stop();
        player_->shutdown();
    }
}

bool av_player_impl::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    // CRITICAL: GstPlayer::init() must be called from main thread
    // In GNU Radio, start() is called from the main thread before work()

    // Initialize GStreamer player
    ::atsc3::av::GstPlayerConfig config;
    config.enable_video = enable_video_;
    config.enable_audio = enable_audio_;

    player_ = std::make_unique<::atsc3::av::GstPlayer>();
    if (!player_->init(config)) {
        // Log error but continue - player may not be available
        player_.reset();
    } else {
        // Set callbacks
        player_->set_state_callback(
            [this](::atsc3::av::PlayerState state) { on_state_change(state); });

        player_->set_volume(volume_);
    }

    // Initialize video decoder
    if (enable_video_) {
        video_decoder_ = std::make_unique<::atsc3::av::HevcDecoder>();
        if (!video_decoder_->init()) {
            video_decoder_.reset();
        }
    }

    // Initialize audio decoder
    if (enable_audio_) {
        ::atsc3::av::AudioDecoderConfig audio_config;
        audio_config.codec = ::atsc3::av::AudioCodecType::AAC;

        audio_decoder_ = std::make_unique<::atsc3::av::AudioDecoder>();
        if (!audio_decoder_->init(audio_config)) {
            audio_decoder_.reset();
        }
    }

    initialized_ = true;

    // Auto-start playback
    if (player_) {
        player_->play();
    }

    return true;
}

bool av_player_impl::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (player_) {
        player_->stop();
        player_->shutdown();
        player_.reset();
    }

    if (video_decoder_) {
        video_decoder_->shutdown();
        video_decoder_.reset();
    }

    if (audio_decoder_) {
        audio_decoder_->shutdown();
        audio_decoder_.reset();
    }

    initialized_ = false;

    return true;
}

int av_player_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                         gr_vector_void_star& /* output_items */) {
    const uint8_t* video_in = static_cast<const uint8_t*>(input_items[0]);
    const uint8_t* audio_in = static_cast<const uint8_t*>(input_items[1]);

    if (!initialized_) {
        return noutput_items;
    }

    // Process video input
    if (enable_video_ && noutput_items > 0) {
        // Get PTS from stream tags if available
        int64_t pts_ns = 0;  // Would extract from tags in real implementation

        if (player_ && player_->is_initialized()) {
            // Push directly to GStreamer player (handles decode internally)
            if (player_->push_video(video_in, static_cast<size_t>(noutput_items), pts_ns)) {
                video_frames_++;
            }
        } else if (video_decoder_) {
            // Decode with FFmpeg and handle frames
            video_decoder_->decode(video_in, static_cast<size_t>(noutput_items), pts_ns);
            video_frames_++;
        }
    }

    // Process audio input
    if (enable_audio_ && noutput_items > 0) {
        int64_t pts_ns = 0;

        if (player_ && player_->is_initialized()) {
            // Push directly to GStreamer player
            if (player_->push_audio(audio_in, static_cast<size_t>(noutput_items), pts_ns)) {
                audio_samples_ += noutput_items;
            }
        } else if (audio_decoder_) {
            // Decode with FFmpeg
            audio_decoder_->decode(audio_in, static_cast<size_t>(noutput_items), pts_ns);
            audio_samples_ += noutput_items;
        }
    }

    // Process GStreamer messages (needed for state updates)
    if (player_) {
        player_->process_messages();
    }

    return noutput_items;
}

bool av_player_impl::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_) {
        return player_->play();
    }
    return false;
}

bool av_player_impl::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_) {
        return player_->pause();
    }
    return false;
}

void av_player_impl::set_volume(float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::max(0.0f, std::min(1.0f, volume));
    if (player_) {
        player_->set_volume(volume_);
    }
}

PlayerState av_player_impl::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_) {
        auto state = player_->get_state();
        return static_cast<PlayerState>(static_cast<uint8_t>(state));
    }
    return PlayerState::STOPPED;
}

void av_player_impl::on_state_change(::atsc3::av::PlayerState state) {
    auto gr_state = static_cast<PlayerState>(static_cast<uint8_t>(state));
    auto msg = state_to_pmt(gr_state);
    message_port_pub(port_state_, msg);
}

pmt::pmt_t av_player_impl::state_to_pmt(PlayerState state) const {
    pmt::pmt_t dict = pmt::make_dict();

    const char* state_str = "unknown";
    switch (state) {
        case PlayerState::STOPPED:
            state_str = "stopped";
            break;
        case PlayerState::PLAYING:
            state_str = "playing";
            break;
        case PlayerState::PAUSED:
            state_str = "paused";
            break;
        case PlayerState::BUFFERING:
            state_str = "buffering";
            break;
        case PlayerState::ERROR:
            state_str = "error";
            break;
    }

    dict = pmt::dict_add(dict, pmt::mp("state"), pmt::intern(state_str));
    dict = pmt::dict_add(dict, pmt::mp("video_frames"), pmt::from_uint64(video_frames_));
    dict = pmt::dict_add(dict, pmt::mp("audio_samples"), pmt::from_uint64(audio_samples_));

    return dict;
}

}  // namespace atsc3
}  // namespace gr
