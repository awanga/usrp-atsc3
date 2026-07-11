// gst_player.cc — GStreamer Player implementation
//
// ATSC 3.0 A/V playback pipeline using GStreamer

#include "gst_player.h"

#include <cstring>
#include <glib.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace atsc3 {
namespace av {

GstPlayer::GstPlayer() = default;

GstPlayer::~GstPlayer() {
    shutdown();
}

bool GstPlayer::init(const GstPlayerConfig& config) {
    if (initialized_.load()) {
        return true;
    }

    config_ = config;

    // Initialize GStreamer
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        if (error != nullptr) {
            if (error_callback_) {
                error_callback_(std::string("GStreamer init failed: ") + error->message);
            }
            g_error_free(error);
        }
        return false;
    }

    if (!create_pipeline()) {
        return false;
    }

    // Start GMainLoop event thread for async message processing
    start_event_loop();

    initialized_.store(true);
    state_.store(PlayerState::STOPPED);

    return true;
}

void GstPlayer::shutdown() {
    if (!initialized_.load()) {
        return;
    }

    stop();
    stop_event_loop();
    destroy_pipeline();
    initialized_.store(false);
}

bool GstPlayer::play() {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (error_callback_) {
            error_callback_("Failed to start playback");
        }
        return false;
    }

    update_state(PlayerState::PLAYING);
    return true;
}

bool GstPlayer::pause() {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        return false;
    }

    update_state(PlayerState::PAUSED);
    return true;
}

bool GstPlayer::stop() {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return false;
    }

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    update_state(PlayerState::STOPPED);
    return true;
}

bool GstPlayer::push_video(const uint8_t* data, size_t size, int64_t pts_ns) {
    if (!initialized_.load() || video_appsrc_ == nullptr || data == nullptr || size == 0) {
        return false;
    }

    // Auto-play on first data if enabled
    if (auto_play_on_data_.load() && !first_data_received_.exchange(true)) {
        play();
    }

    // Create buffer
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (buffer == nullptr) {
        return false;
    }

    // Copy data to buffer
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return false;
    }
    std::memcpy(map.data, data, size);
    gst_buffer_unmap(buffer, &map);

    // Set timestamp
    if (pts_ns >= 0) {
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(pts_ns);
    }

    // Push to appsrc
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(video_appsrc_), buffer);

    return ret == GST_FLOW_OK;
}

bool GstPlayer::push_audio(const uint8_t* data, size_t size, int64_t pts_ns,
                           StreamType stream_type) {
    if (!initialized_.load() || audio_appsrc_ == nullptr || data == nullptr || size == 0) {
        return false;
    }

    // Auto-play on first data if enabled
    if (auto_play_on_data_.load() && !first_data_received_.exchange(true)) {
        play();
    }

    (void)stream_type;  // Currently using same appsrc for all audio types

    // Create buffer
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (buffer == nullptr) {
        return false;
    }

    // Copy data
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return false;
    }
    std::memcpy(map.data, data, size);
    gst_buffer_unmap(buffer, &map);

    // Set timestamp
    if (pts_ns >= 0) {
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(pts_ns);
    }

    // Push to appsrc
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(audio_appsrc_), buffer);

    return ret == GST_FLOW_OK;
}

void GstPlayer::signal_eos() {
    if (!initialized_.load()) {
        return;
    }

    if (video_appsrc_ != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(video_appsrc_));
    }
    if (audio_appsrc_ != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(audio_appsrc_));
    }
}

void GstPlayer::flush() {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return;
    }

    // Send flush events
    gst_element_send_event(pipeline_, gst_event_new_flush_start());
    gst_element_send_event(pipeline_, gst_event_new_flush_stop(TRUE));
}

GstPlayerStats GstPlayer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void GstPlayer::set_error_callback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

void GstPlayer::set_state_callback(StateCallback callback) {
    state_callback_ = std::move(callback);
}

void GstPlayer::set_eos_callback(EosCallback callback) {
    eos_callback_ = std::move(callback);
}

bool GstPlayer::seek(int64_t position_ns) {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return false;
    }

    return gst_element_seek_simple(
        pipeline_, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), position_ns);
}

int64_t GstPlayer::get_position() const {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return -1;
    }

    gint64 position;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &position)) {
        return position;
    }
    return -1;
}

int64_t GstPlayer::get_duration() const {
    if (!initialized_.load() || pipeline_ == nullptr) {
        return -1;
    }

    gint64 duration;
    if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration)) {
        return duration;
    }
    return -1;
}

void GstPlayer::set_volume(float volume) {
    volume_.store(std::max(0.0f, std::min(1.0f, volume)));

    if (pipeline_ != nullptr) {
        g_object_set(pipeline_, "volume", static_cast<double>(volume_.load()), nullptr);
    }
}

float GstPlayer::get_volume() const {
    return volume_.load();
}

bool GstPlayer::process_messages() {
    if (!initialized_.load() || bus_ == nullptr) {
        return true;
    }

    GstMessage* msg;
    while ((msg = gst_bus_pop(bus_)) != nullptr) {
        handle_message(msg);
        gst_message_unref(msg);
    }

    return state_.load() != PlayerState::ERROR;
}

// Static callback for bus messages (called from GMainLoop thread)
// Signature matches GstBusFunc for gst_bus_add_watch
gboolean GstPlayer::bus_callback(GstBus* /* bus */, GstMessage* msg, void* user_data) {
    auto* player = static_cast<GstPlayer*>(user_data);
    player->handle_message(msg);
    return TRUE;  // Keep watching
}

void GstPlayer::start_event_loop() {
    if (bus_ == nullptr) {
        return;
    }

    // Create a dedicated context for this player
    main_context_ = g_main_context_new();
    main_loop_ = g_main_loop_new(main_context_, FALSE);

    // Add bus watch using the standard GStreamer API
    // gst_bus_add_watch uses the default main context, so we use gst_bus_add_watch_full
    // with our custom context
    GSource* bus_source = gst_bus_create_watch(bus_);
    g_source_set_callback(
        bus_source, G_SOURCE_FUNC(+[](GstBus* bus, GstMessage* msg, gpointer data) -> gboolean {
            return bus_callback(bus, msg, data);
        }),
        this, nullptr);
    bus_watch_id_ = g_source_attach(bus_source, main_context_);
    g_source_unref(bus_source);

    // Start event thread
    event_thread_ = std::thread([this]() {
        g_main_context_push_thread_default(main_context_);
        g_main_loop_run(main_loop_);
        g_main_context_pop_thread_default(main_context_);
    });
}

void GstPlayer::stop_event_loop() {
    // Signal loop to quit
    if (main_loop_ != nullptr) {
        g_main_loop_quit(main_loop_);
    }

    // Wait for thread to finish
    if (event_thread_.joinable()) {
        event_thread_.join();
    }

    // Clean up
    if (main_loop_ != nullptr) {
        g_main_loop_unref(main_loop_);
        main_loop_ = nullptr;
    }
    if (main_context_ != nullptr) {
        g_main_context_unref(main_context_);
        main_context_ = nullptr;
    }
    bus_watch_id_ = 0;
}

bool GstPlayer::create_pipeline() {
    // Create main pipeline
    pipeline_ = gst_pipeline_new("atsc3_player");
    if (pipeline_ == nullptr) {
        if (error_callback_) {
            error_callback_("Failed to create pipeline");
        }
        return false;
    }

    // Get bus for messages
    bus_ = gst_element_get_bus(pipeline_);

    // Create video branch if enabled
    if (config_.enable_video) {
        if (!create_video_branch()) {
            destroy_pipeline();
            return false;
        }
    }

    // Create audio branch if enabled
    if (config_.enable_audio) {
        if (!create_audio_branch()) {
            destroy_pipeline();
            return false;
        }
    }

    return true;
}

void GstPlayer::destroy_pipeline() {
    if (bus_ != nullptr) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }

    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    video_appsrc_ = nullptr;
    audio_appsrc_ = nullptr;
}

bool GstPlayer::create_video_branch() {
    // Create elements
    video_appsrc_ = create_element("appsrc", "video_src");
    GstElement* h265parse = create_element("h265parse", "h265parse");
    GstElement* decoder = create_element("avdec_h265", "video_decoder");
    GstElement* convert = create_element("videoconvert", "video_convert");
    GstElement* sink = create_element(config_.video_sink.c_str(), "video_sink");

    if (video_appsrc_ == nullptr || h265parse == nullptr || decoder == nullptr ||
        convert == nullptr || sink == nullptr) {
        if (error_callback_) {
            error_callback_("Failed to create video elements");
        }
        return false;
    }

    // Configure appsrc
    g_object_set(video_appsrc_, "stream-type", 0,  // GST_APP_STREAM_TYPE_STREAM
                 "format", GST_FORMAT_TIME, "is-live", TRUE, nullptr);

    // Set caps for HEVC
    GstCaps* caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING,
                                        "byte-stream", "alignment", G_TYPE_STRING, "au", nullptr);
    g_object_set(video_appsrc_, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Add to pipeline
    gst_bin_add_many(GST_BIN(pipeline_), video_appsrc_, h265parse, decoder, convert, sink, nullptr);

    // Link elements
    if (!gst_element_link_many(video_appsrc_, h265parse, decoder, convert, sink, nullptr)) {
        if (error_callback_) {
            error_callback_("Failed to link video elements");
        }
        return false;
    }

    // Configure sync
    if (config_.sync_to_clock) {
        g_object_set(sink, "sync", TRUE, nullptr);
    }

    return true;
}

bool GstPlayer::create_audio_branch() {
    // Create elements
    audio_appsrc_ = create_element("appsrc", "audio_src");
    GstElement* parser = create_element("aacparse", "aac_parse");
    GstElement* decoder = create_element("avdec_aac", "audio_decoder");
    GstElement* convert = create_element("audioconvert", "audio_convert");
    GstElement* sink = create_element(config_.audio_sink.c_str(), "audio_sink");

    if (audio_appsrc_ == nullptr || parser == nullptr || decoder == nullptr || convert == nullptr ||
        sink == nullptr) {
        if (error_callback_) {
            error_callback_("Failed to create audio elements");
        }
        return false;
    }

    // Configure appsrc
    g_object_set(audio_appsrc_, "stream-type", 0, "format", GST_FORMAT_TIME, "is-live", TRUE,
                 nullptr);

    // Set caps for AAC
    GstCaps* caps = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format",
                                        G_TYPE_STRING, "raw", nullptr);
    g_object_set(audio_appsrc_, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Add to pipeline
    gst_bin_add_many(GST_BIN(pipeline_), audio_appsrc_, parser, decoder, convert, sink, nullptr);

    // Link elements
    if (!gst_element_link_many(audio_appsrc_, parser, decoder, convert, sink, nullptr)) {
        if (error_callback_) {
            error_callback_("Failed to link audio elements");
        }
        return false;
    }

    // Configure sync
    if (config_.sync_to_clock) {
        g_object_set(sink, "sync", TRUE, nullptr);
    }

    return true;
}

void GstPlayer::handle_message(GstMessage* msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug_info = nullptr;
            gst_message_parse_error(msg, &err, &debug_info);

            std::string error_msg = err->message;
            if (debug_info != nullptr) {
                error_msg += " (";
                error_msg += debug_info;
                error_msg += ")";
            }

            if (error_callback_) {
                error_callback_(error_msg);
            }

            update_state(PlayerState::ERROR);

            g_error_free(err);
            g_free(debug_info);
            break;
        }

        case GST_MESSAGE_EOS: {
            if (eos_callback_) {
                eos_callback_();
            }
            update_state(PlayerState::STOPPED);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

                switch (new_state) {
                    case GST_STATE_PLAYING:
                        update_state(PlayerState::PLAYING);
                        break;
                    case GST_STATE_PAUSED:
                        update_state(PlayerState::PAUSED);
                        break;
                    case GST_STATE_NULL:
                    case GST_STATE_READY:
                        update_state(PlayerState::STOPPED);
                        break;
                    default:
                        break;
                }
            }
            break;
        }

        case GST_MESSAGE_BUFFERING: {
            gint percent;
            gst_message_parse_buffering(msg, &percent);

            if (percent < 100) {
                update_state(PlayerState::BUFFERING);
            } else if (state_.load() == PlayerState::BUFFERING) {
                update_state(PlayerState::PLAYING);
            }

            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.buffer_level_ms = percent * config_.buffer_size_ms / 100;
            }
            break;
        }

        case GST_MESSAGE_QOS: {
            gboolean live;
            guint64 running_time, stream_time, timestamp, duration;
            gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);

            // Check if it's a dropped frame
            gint64 jitter;
            gdouble proportion;
            gint quality;
            gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);

            if (jitter > 0) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.video_frames_dropped++;
            }
            break;
        }

        default:
            break;
    }
}

void GstPlayer::update_state(PlayerState new_state) {
    PlayerState old_state = state_.exchange(new_state);

    if (old_state != new_state && state_callback_) {
        state_callback_(new_state);
    }
}

GstElement* GstPlayer::create_element(const char* factory, const char* name) {
    GstElement* element = gst_element_factory_make(factory, name);
    if (element == nullptr && error_callback_) {
        error_callback_(std::string("Failed to create element: ") + factory);
    }
    return element;
}

}  // namespace av
}  // namespace atsc3
