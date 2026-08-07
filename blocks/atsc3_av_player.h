// atsc3_av_player.h — GNU Radio ATSC 3.0 A/V Player Block
//
// AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(frame)
// Input 0: Video ES (HEVC NALs)
// Input 1: Audio ES (AC-4/AAC frames)
// Output: Message port for state changes

#ifndef INCLUDED_ATSC3_AV_PLAYER_H
#define INCLUDED_ATSC3_AV_PLAYER_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

// Player state enum (mirrors av/gst_player.h)
enum class PlayerState : uint8_t { STOPPED = 0, PLAYING = 1, PAUSED = 2, BUFFERING = 3, ERROR = 4 };

class av_player : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<av_player> sptr;

    /*!
     * \brief Create ATSC 3.0 A/V player block
     * \param enable_video Enable video playback
     * \param enable_audio Enable audio playback
     * \param volume Initial volume (0.0 - 1.0)
     * \return Shared pointer to new instance
     */
    static sptr make(bool enable_video = true, bool enable_audio = true, float volume = 1.0f);

    /*!
     * \brief Start playback
     */
    virtual bool play() = 0;

    /*!
     * \brief Pause playback
     */
    virtual bool pause() = 0;

    /*!
     * \brief Stop playback
     */
    virtual bool stop() = 0;

    /*!
     * \brief Set volume
     * \param volume Volume level (0.0 - 1.0)
     */
    virtual void set_volume(float volume) = 0;

    /*!
     * \brief Get current volume
     */
    virtual float get_volume() const = 0;

    /*!
     * \brief Get current player state
     */
    virtual PlayerState get_state() const = 0;

    /*!
     * \brief Get video frames rendered
     */
    virtual uint64_t get_video_frames() const = 0;

    /*!
     * \brief Get audio samples played
     */
    virtual uint64_t get_audio_samples() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_AV_PLAYER_H */
