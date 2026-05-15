#pragma once

// HEVC Decoder — FFmpeg-based H.265/HEVC video decoder
//
// Decodes HEVC elementary stream to raw YUV420 frames.
// Designed for ATSC 3.0 video streams using Main/Main10 profiles.
//
// Threading model:
//   - Decoder runs in its own thread (can be configured)
//   - Output frames delivered via ring buffer
//   - Thread-safe frame retrieval from consumer
//
// Memory model:
//   - Fixed frame buffer pool allocated at init
//   - No dynamic allocation during steady-state decode

#include "ring_buffer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace atsc3 {
namespace av {

// Video frame format
enum class PixelFormat : uint8_t {
    YUV420P = 0,    // Planar YUV 4:2:0, 8-bit
    YUV420P10 = 1,  // Planar YUV 4:2:0, 10-bit
    NV12 = 2,       // Semi-planar YUV 4:2:0, 8-bit
    UNKNOWN = 255
};

// Decoded video frame
struct VideoFrame {
    // Frame dimensions
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::UNKNOWN;

    // Plane pointers (Y, U, V for planar formats)
    std::array<uint8_t*, 4> data = {};
    std::array<size_t, 4> linesize = {};

    // Timing
    int64_t pts = 0;        // Presentation timestamp
    int64_t dts = 0;        // Decode timestamp
    uint32_t duration = 0;  // Frame duration

    // Frame metadata
    bool key_frame = false;
    uint32_t frame_number = 0;

    // Buffer ownership (internal use)
    void* opaque = nullptr;

    // Data size for Y plane (main plane)
    size_t y_size() const {
        return linesize[0] * height;
    }

    // Total frame data size
    size_t total_size() const {
        if (format == PixelFormat::YUV420P || format == PixelFormat::YUV420P10) {
            return y_size() + 2 * (linesize[1] * height / 2);
        }
        return 0;
    }

    bool valid() const {
        return width > 0 && height > 0 && data[0] != nullptr;
    }
};

// Decoder statistics
struct HevcDecoderStats {
    uint64_t frames_decoded = 0;
    uint64_t frames_dropped = 0;
    uint64_t decode_errors = 0;
    uint64_t packets_received = 0;
    uint64_t bytes_received = 0;
    double average_decode_time_ms = 0.0;
};

// Decoder configuration
struct HevcDecoderConfig {
    // Threading
    int num_threads = 0;          // 0 = auto (based on CPU cores)
    bool threaded_decode = true;  // Use FFmpeg internal threading

    // Output buffer
    static constexpr size_t OUTPUT_BUFFER_SIZE = 8;  // Power of 2

    // Low-latency mode (disable B-frame reordering)
    bool low_latency = false;

    // Maximum frame dimensions (for buffer pre-allocation)
    uint32_t max_width = 3840;
    uint32_t max_height = 2160;
};

// Frame callback type
using FrameCallback = std::function<void(const VideoFrame&)>;

// HEVC Decoder
//
// Decodes HEVC elementary stream using FFmpeg libavcodec.
// Thread-safe for concurrent decode/consume operations.
//
// Usage:
//   HevcDecoder decoder;
//   if (!decoder.init()) { handle error }
//
//   // Push encoded data
//   decoder.decode(nal_data, nal_size, pts);
//
//   // Retrieve decoded frames
//   while (auto frame = decoder.get_frame()) {
//       // Process frame
//   }
//
class HevcDecoder {
public:
    HevcDecoder();
    ~HevcDecoder();

    // Non-copyable
    HevcDecoder(const HevcDecoder&) = delete;
    HevcDecoder& operator=(const HevcDecoder&) = delete;

    // Initialize decoder
    // Returns true on success
    bool init(const HevcDecoderConfig& config = HevcDecoderConfig());

    // Shutdown decoder
    void shutdown();

    // Check if initialized
    bool is_initialized() const {
        return initialized_.load();
    }

    // Decode NAL unit or access unit
    // data: HEVC NAL data (with or without start codes)
    // size: Data size in bytes
    // pts: Presentation timestamp (optional, -1 if unknown)
    // Returns: true if data accepted for decode
    bool decode(const uint8_t* data, size_t size, int64_t pts = -1);

    // Get next decoded frame
    // Returns frame or nullopt if no frame available
    std::optional<VideoFrame> get_frame();

    // Set callback for decoded frames (alternative to polling)
    void set_frame_callback(FrameCallback callback);

    // Flush decoder (call at end of stream)
    void flush();

    // Get decoder statistics
    HevcDecoderStats get_stats() const;

    // Reset statistics
    void reset_stats();

    // Get current output format
    bool get_output_format(uint32_t& width, uint32_t& height, PixelFormat& format) const;

private:
    HevcDecoderConfig config_;
    std::atomic<bool> initialized_{false};

    // FFmpeg context
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* decode_frame_ = nullptr;
    AVPacket* decode_packet_ = nullptr;

    // Output buffer (thread-safe ring buffer)
    RingBuffer<VideoFrame, HevcDecoderConfig::OUTPUT_BUFFER_SIZE> output_buffer_;

    // Frame callback
    FrameCallback frame_callback_;

    // Statistics
    mutable HevcDecoderStats stats_;
    std::atomic<uint32_t> frame_number_{0};

    // Frame buffer pool
    std::vector<std::vector<uint8_t>> frame_pool_;
    size_t frame_pool_idx_ = 0;

    // Internal helpers
    bool init_codec();
    void cleanup_codec();
    bool send_packet(const uint8_t* data, size_t size, int64_t pts);
    bool receive_frames();
    VideoFrame convert_frame(AVFrame* frame);
    uint8_t* get_frame_buffer(size_t size);
};

}  // namespace av
}  // namespace atsc3
