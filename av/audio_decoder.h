#pragma once

// Audio Decoder — FFmpeg-based AC-4/AAC audio decoder
//
// Decodes AC-4 or HE-AAC elementary stream to PCM float32.
// Designed for ATSC 3.0 audio streams.
//
// Threading model:
//   - Decoder runs in decode thread context
//   - Output samples delivered via ring buffer
//   - Thread-safe sample retrieval from consumer
//
// Memory model:
//   - Fixed sample buffer pool allocated at init
//   - No dynamic allocation during steady-state decode

#include "ring_buffer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace atsc3 {
namespace av {

// Audio codec type
enum class AudioCodecType : uint8_t {
    UNKNOWN = 0,
    AC4 = 1,    // Dolby AC-4
    AAC = 2,    // HE-AAC / AAC-LC
    MPEG_H = 3  // MPEG-H 3D Audio
};

// Audio sample format
enum class SampleFormat : uint8_t {
    FLOAT32 = 0,   // Interleaved float32 [-1.0, 1.0]
    INT16 = 1,     // Interleaved int16
    INT32 = 2,     // Interleaved int32
    FLOAT32P = 3,  // Planar float32
    UNKNOWN = 255
};

// Decoded audio frame
struct AudioFrame {
    // Sample data (interleaved)
    std::vector<float> samples;

    // Format info
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    SampleFormat format = SampleFormat::FLOAT32;

    // Timing
    int64_t pts = 0;           // Presentation timestamp
    uint32_t num_samples = 0;  // Samples per channel
    uint32_t duration_ms = 0;  // Duration in milliseconds

    // Metadata
    uint32_t frame_number = 0;

    bool valid() const {
        return sample_rate > 0 && channels > 0 && num_samples > 0;
    }

    // Total sample count (samples_per_channel * channels)
    size_t total_samples() const {
        return static_cast<size_t>(num_samples) * channels;
    }

    // Duration in seconds
    double duration_sec() const {
        return sample_rate > 0 ? static_cast<double>(num_samples) / sample_rate : 0.0;
    }
};

// Decoder statistics
struct AudioDecoderStats {
    uint64_t frames_decoded = 0;
    uint64_t frames_dropped = 0;
    uint64_t decode_errors = 0;
    uint64_t packets_received = 0;
    uint64_t bytes_received = 0;
    uint64_t samples_decoded = 0;
    double average_decode_time_ms = 0.0;
};

// Decoder configuration
struct AudioDecoderConfig {
    // Codec to use
    AudioCodecType codec = AudioCodecType::AAC;

    // Output format
    SampleFormat output_format = SampleFormat::FLOAT32;
    uint32_t output_sample_rate = 48000;  // 0 = match source
    uint8_t output_channels = 0;          // 0 = match source

    // Buffer configuration
    static constexpr size_t OUTPUT_BUFFER_SIZE = 16;  // Power of 2

    // Maximum expected samples per frame
    static constexpr size_t MAX_SAMPLES_PER_FRAME = 4096;
    static constexpr size_t MAX_CHANNELS = 8;
};

// Frame callback type
using AudioFrameCallback = std::function<void(const AudioFrame&)>;

// Audio Decoder
//
// Decodes AC-4 or AAC elementary stream using FFmpeg libavcodec.
// Converts to float32 PCM output format.
//
// Usage:
//   AudioDecoderConfig config;
//   config.codec = AudioCodecType::AC4;
//
//   AudioDecoder decoder;
//   if (!decoder.init(config)) { handle error }
//
//   // Push encoded data
//   decoder.decode(frame_data, frame_size, pts);
//
//   // Retrieve decoded samples
//   while (auto frame = decoder.get_frame()) {
//       // Process audio samples
//   }
//
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Non-copyable
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Initialize decoder
    // Returns true on success
    bool init(const AudioDecoderConfig& config = AudioDecoderConfig());

    // Shutdown decoder
    void shutdown();

    // Check if initialized
    bool is_initialized() const {
        return initialized_.load();
    }

    // Decode audio frame
    // data: Encoded audio data
    // size: Data size in bytes
    // pts: Presentation timestamp (optional)
    // Returns: true if data accepted for decode
    bool decode(const uint8_t* data, size_t size, int64_t pts = -1);

    // Get next decoded frame
    // Returns frame or nullopt if no frame available
    std::optional<AudioFrame> get_frame();

    // Set callback for decoded frames
    void set_frame_callback(AudioFrameCallback callback);

    // Flush decoder (call at end of stream)
    void flush();

    // Get decoder statistics
    AudioDecoderStats get_stats() const;

    // Reset statistics
    void reset_stats();

    // Get current output format
    bool get_output_format(uint32_t& sample_rate, uint8_t& channels) const;

    // Get detected codec
    AudioCodecType get_detected_codec() const {
        return detected_codec_;
    }

private:
    AudioDecoderConfig config_;
    std::atomic<bool> initialized_{false};
    AudioCodecType detected_codec_ = AudioCodecType::UNKNOWN;

    // FFmpeg context
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* decode_frame_ = nullptr;
    AVPacket* decode_packet_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;

    // Output buffer
    RingBuffer<AudioFrame, AudioDecoderConfig::OUTPUT_BUFFER_SIZE> output_buffer_;

    // Frame callback
    AudioFrameCallback frame_callback_;

    // Statistics
    mutable AudioDecoderStats stats_;
    std::atomic<uint32_t> frame_number_{0};

    // Resampler output buffer
    std::vector<float> resample_buffer_;

    // Internal helpers
    bool init_codec(AudioCodecType codec);
    void cleanup_codec();
    bool init_resampler();
    bool send_packet(const uint8_t* data, size_t size, int64_t pts);
    bool receive_frames();
    AudioFrame convert_frame(AVFrame* frame);
};

}  // namespace av
}  // namespace atsc3
