#pragma once

// GStreamer Player — ATSC 3.0 A/V Playback Pipeline
//
// Plays decoded HEVC video and AC-4/AAC audio using GStreamer.
// Handles A/V synchronization via GStreamer clock.
//
// Threading constraints:
//   - Pipeline MUST be created on main thread
//   - appsrc push can be called from any thread
//   - State changes must be from main thread
//
// Pipeline structure:
//   Video: appsrc → h265parse → avdec_h265 → videoconvert → autovideosink
//   Audio: appsrc → aacparse → avdec_aac → audioconvert → autoaudiosink

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations for GStreamer types
typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GMainLoop GMainLoop;
typedef struct _GMainContext GMainContext;
typedef int gboolean;
typedef unsigned int guint;

namespace atsc3 {
namespace av {

// Player state
enum class PlayerState : uint8_t { STOPPED = 0, PLAYING = 1, PAUSED = 2, BUFFERING = 3, ERROR = 4 };

// Stream type
enum class StreamType : uint8_t { VIDEO_HEVC = 0, AUDIO_AAC = 1, AUDIO_AC4 = 2 };

// Player configuration
struct GstPlayerConfig {
    // Video output
    bool enable_video = true;
    std::string video_sink = "autovideosink";  // Or "ximagesink", "waylandsink", etc.

    // Audio output
    bool enable_audio = true;
    std::string audio_sink = "autoaudiosink";  // Or "pulsesink", "alsasink", etc.

    // Buffering
    uint32_t buffer_size_ms = 500;   // Buffer size in milliseconds
    uint32_t max_lateness_ms = 100;  // Max frame lateness before drop

    // Sync
    bool sync_to_clock = true;  // Enable A/V sync

    // Debug
    bool enable_debug = false;  // Enable GStreamer debug output
};

// Player statistics
struct GstPlayerStats {
    uint64_t video_frames_rendered = 0;
    uint64_t video_frames_dropped = 0;
    uint64_t audio_samples_played = 0;
    uint64_t audio_underruns = 0;
    int64_t current_position_ms = 0;
    int64_t buffer_level_ms = 0;
};

// Error callback type
using ErrorCallback = std::function<void(const std::string& message)>;

// State change callback
using StateCallback = std::function<void(PlayerState state)>;

// End of stream callback
using EosCallback = std::function<void()>;

// GStreamer Player
//
// Creates and manages GStreamer pipeline for A/V playback.
//
// IMPORTANT: The init() method MUST be called from the main thread.
// GStreamer requires pipeline creation on the main thread.
//
// Usage:
//   GstPlayer player;
//
//   // Initialize on main thread
//   if (!player.init()) { handle error }
//
//   // Start playback
//   player.play();
//
//   // Push encoded data (can be from any thread)
//   player.push_video(nal_data, nal_size, pts);
//   player.push_audio(audio_data, audio_size, pts);
//
//   // Stop when done
//   player.stop();
//
class GstPlayer {
public:
    GstPlayer();
    ~GstPlayer();

    // Non-copyable
    GstPlayer(const GstPlayer&) = delete;
    GstPlayer& operator=(const GstPlayer&) = delete;

    // Initialize player (MUST be called from main thread)
    // Returns true on success
    bool init(const GstPlayerConfig& config = GstPlayerConfig());

    // Shutdown player
    void shutdown();

    // Check if initialized
    bool is_initialized() const {
        return initialized_.load();
    }

    // Start playback
    bool play();

    // Pause playback
    bool pause();

    // Stop playback
    bool stop();

    // Get current state
    PlayerState get_state() const {
        return state_.load();
    }

    // Push video NAL data (can be called from any thread)
    // data: HEVC NAL unit data (with start codes)
    // size: Data size in bytes
    // pts: Presentation timestamp in nanoseconds
    // Returns: true if data was pushed successfully
    bool push_video(const uint8_t* data, size_t size, int64_t pts_ns);

    // Push audio data (can be called from any thread)
    // data: Encoded audio frame data
    // size: Data size in bytes
    // pts: Presentation timestamp in nanoseconds
    // stream_type: AUDIO_AAC or AUDIO_AC4
    // Returns: true if data was pushed successfully
    bool push_audio(const uint8_t* data, size_t size, int64_t pts_ns,
                    StreamType stream_type = StreamType::AUDIO_AAC);

    // Signal end of stream
    void signal_eos();

    // Flush pipelines (for seeking or stream discontinuity)
    void flush();

    // Get statistics
    GstPlayerStats get_stats() const;

    // Set callbacks
    void set_error_callback(ErrorCallback callback);
    void set_state_callback(StateCallback callback);
    void set_eos_callback(EosCallback callback);

    // Seek to position (in nanoseconds)
    bool seek(int64_t position_ns);

    // Get current position (in nanoseconds)
    int64_t get_position() const;

    // Get duration (if known, -1 otherwise)
    int64_t get_duration() const;

    // Set volume (0.0 to 1.0)
    void set_volume(float volume);

    // Get volume
    float get_volume() const;

    // Enable auto-play mode: play() is called automatically when first data arrives
    // This avoids the race condition of calling play() before data is available
    void set_auto_play(bool enable) {
        auto_play_on_data_.store(enable);
    }

    // Process pending messages (call periodically from main thread)
    // Returns false if error occurred
    bool process_messages();

private:
    GstPlayerConfig config_;
    std::atomic<bool> initialized_{false};
    std::atomic<PlayerState> state_{PlayerState::STOPPED};

    // GStreamer elements
    GstElement* pipeline_ = nullptr;
    GstElement* video_appsrc_ = nullptr;
    GstElement* audio_appsrc_ = nullptr;
    GstBus* bus_ = nullptr;

    // Callbacks
    ErrorCallback error_callback_;
    StateCallback state_callback_;
    EosCallback eos_callback_;

    // Statistics
    mutable GstPlayerStats stats_;
    mutable std::mutex stats_mutex_;

    // Volume
    std::atomic<float> volume_{1.0f};

    // Auto-play: defer play() until first data arrives
    std::atomic<bool> auto_play_on_data_{false};
    std::atomic<bool> first_data_received_{false};

    // GMainLoop for event processing (runs in separate thread)
    GMainLoop* main_loop_ = nullptr;
    GMainContext* main_context_ = nullptr;
    std::thread event_thread_;
    guint bus_watch_id_ = 0;

    // Event loop management
    void start_event_loop();
    void stop_event_loop();
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, void* user_data);

    // Internal helpers
    bool create_pipeline();
    void destroy_pipeline();
    bool create_video_branch();
    bool create_audio_branch();
    bool link_elements();
    void handle_message(GstMessage* msg);
    void update_state(PlayerState new_state);
    GstElement* create_element(const char* factory, const char* name);
};

}  // namespace av
}  // namespace atsc3
