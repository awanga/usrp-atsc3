// hevc_decoder.cc — HEVC Decoder implementation
//
// FFmpeg-based H.265/HEVC decoder for ATSC 3.0 video

#include "hevc_decoder.h"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
}

namespace atsc3 {
namespace av {

HevcDecoder::HevcDecoder() = default;

HevcDecoder::~HevcDecoder() {
    shutdown();
}

bool HevcDecoder::init(const HevcDecoderConfig& config) {
    if (initialized_.load()) {
        return true;  // Already initialized
    }

    config_ = config;

    if (!init_codec()) {
        return false;
    }

    // Pre-allocate frame buffer pool
    size_t max_frame_size = config_.max_width * config_.max_height * 3 / 2;  // YUV420
    frame_pool_.resize(HevcDecoderConfig::OUTPUT_BUFFER_SIZE * 2);
    for (auto& buf : frame_pool_) {
        buf.resize(max_frame_size);
    }
    frame_pool_idx_ = 0;

    initialized_.store(true);
    return true;
}

void HevcDecoder::shutdown() {
    if (!initialized_.load()) {
        return;
    }

    initialized_.store(false);
    cleanup_codec();
    frame_pool_.clear();
    output_buffer_.clear();
}

bool HevcDecoder::decode(const uint8_t* data, size_t size, int64_t pts) {
    if (!initialized_.load() || data == nullptr || size == 0) {
        return false;
    }

    stats_.packets_received++;
    stats_.bytes_received += size;

    auto start = std::chrono::high_resolution_clock::now();

    if (!send_packet(data, size, pts)) {
        stats_.decode_errors++;
        return false;
    }

    receive_frames();

    auto end = std::chrono::high_resolution_clock::now();
    double decode_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Update running average
    double n = static_cast<double>(stats_.frames_decoded);
    if (n > 0) {
        stats_.average_decode_time_ms = (stats_.average_decode_time_ms * (n - 1) + decode_ms) / n;
    }

    return true;
}

std::optional<VideoFrame> HevcDecoder::get_frame() {
    return output_buffer_.pop();
}

void HevcDecoder::set_frame_callback(FrameCallback callback) {
    frame_callback_ = std::move(callback);
}

void HevcDecoder::flush() {
    if (!initialized_.load() || codec_ctx_ == nullptr) {
        return;
    }

    // Send flush packet
    avcodec_send_packet(codec_ctx_, nullptr);

    // Receive remaining frames
    receive_frames();
}

HevcDecoderStats HevcDecoder::get_stats() const {
    return stats_;
}

void HevcDecoder::reset_stats() {
    stats_ = HevcDecoderStats();
}

bool HevcDecoder::get_output_format(uint32_t& width, uint32_t& height, PixelFormat& format) const {
    if (!initialized_.load() || codec_ctx_ == nullptr) {
        return false;
    }

    width = static_cast<uint32_t>(codec_ctx_->width);
    height = static_cast<uint32_t>(codec_ctx_->height);

    switch (codec_ctx_->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            format = PixelFormat::YUV420P;
            break;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV420P10BE:
            format = PixelFormat::YUV420P10;
            break;
        case AV_PIX_FMT_NV12:
            format = PixelFormat::NV12;
            break;
        default:
            format = PixelFormat::UNKNOWN;
            break;
    }

    return width > 0 && height > 0;
}

bool HevcDecoder::init_codec() {
    // Find HEVC decoder
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (codec == nullptr) {
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (codec_ctx_ == nullptr) {
        return false;
    }

    // Configure threading
    if (config_.num_threads > 0) {
        codec_ctx_->thread_count = config_.num_threads;
    }
    if (config_.threaded_decode) {
        codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    // Low latency options
    if (config_.low_latency) {
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    // Open codec
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    // Allocate frame and packet
    decode_frame_ = av_frame_alloc();
    decode_packet_ = av_packet_alloc();

    if (decode_frame_ == nullptr || decode_packet_ == nullptr) {
        cleanup_codec();
        return false;
    }

    return true;
}

void HevcDecoder::cleanup_codec() {
    if (decode_packet_ != nullptr) {
        av_packet_free(&decode_packet_);
    }
    if (decode_frame_ != nullptr) {
        av_frame_free(&decode_frame_);
    }
    if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
    }
}

bool HevcDecoder::send_packet(const uint8_t* data, size_t size, int64_t pts) {
    decode_packet_->data = const_cast<uint8_t*>(data);
    decode_packet_->size = static_cast<int>(size);
    decode_packet_->pts = pts;
    decode_packet_->dts = pts;

    int ret = avcodec_send_packet(codec_ctx_, decode_packet_);

    // Reset packet for next use
    decode_packet_->data = nullptr;
    decode_packet_->size = 0;

    return ret >= 0 || ret == AVERROR(EAGAIN);
}

bool HevcDecoder::receive_frames() {
    bool got_frame = false;

    while (true) {
        int ret = avcodec_receive_frame(codec_ctx_, decode_frame_);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }

        if (ret < 0) {
            stats_.decode_errors++;
            break;
        }

        // Convert and queue frame
        VideoFrame frame = convert_frame(decode_frame_);

        if (frame.valid()) {
            stats_.frames_decoded++;

            // Try to push to output buffer
            if (!output_buffer_.push(std::move(frame))) {
                stats_.frames_dropped++;
            }

            // Invoke callback if set
            if (frame_callback_) {
                const VideoFrame* queued = output_buffer_.peek();
                if (queued != nullptr) {
                    frame_callback_(*queued);
                }
            }

            got_frame = true;
        }

        av_frame_unref(decode_frame_);
    }

    return got_frame;
}

VideoFrame HevcDecoder::convert_frame(AVFrame* frame) {
    VideoFrame out;

    if (frame == nullptr || frame->data[0] == nullptr) {
        return out;
    }

    out.width = static_cast<uint32_t>(frame->width);
    out.height = static_cast<uint32_t>(frame->height);
    out.pts = frame->pts;
    out.dts = frame->pkt_dts;
    // FFmpeg 6.0+ uses duration and AV_FRAME_FLAG_KEY; older versions use pkt_duration and
    // key_frame
#if LIBAVUTIL_VERSION_MAJOR >= 58
    out.duration = static_cast<uint32_t>(frame->duration);
    out.key_frame = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
#else
    out.duration = static_cast<uint32_t>(frame->pkt_duration);
    out.key_frame = frame->key_frame != 0;
#endif
    out.frame_number = frame_number_.fetch_add(1);

    // Determine format
    switch (frame->format) {
        case AV_PIX_FMT_YUV420P:
            out.format = PixelFormat::YUV420P;
            break;
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV420P10BE:
            out.format = PixelFormat::YUV420P10;
            break;
        case AV_PIX_FMT_NV12:
            out.format = PixelFormat::NV12;
            break;
        default:
            out.format = PixelFormat::UNKNOWN;
            return out;
    }

    // Calculate sizes
    size_t y_size = static_cast<size_t>(frame->linesize[0]) * out.height;
    size_t uv_height = out.height / 2;
    size_t uv_size = static_cast<size_t>(frame->linesize[1]) * uv_height;
    size_t total_size = y_size + 2 * uv_size;

    // Get buffer from pool
    uint8_t* buffer = get_frame_buffer(total_size);
    if (buffer == nullptr) {
        return VideoFrame();  // Return invalid frame
    }

    // Copy Y plane
    out.data[0] = buffer;
    out.linesize[0] = static_cast<size_t>(frame->linesize[0]);
    std::memcpy(out.data[0], frame->data[0], y_size);

    // Copy U plane
    out.data[1] = buffer + y_size;
    out.linesize[1] = static_cast<size_t>(frame->linesize[1]);
    std::memcpy(out.data[1], frame->data[1], uv_size);

    // Copy V plane
    out.data[2] = buffer + y_size + uv_size;
    out.linesize[2] = static_cast<size_t>(frame->linesize[2]);
    std::memcpy(out.data[2], frame->data[2], uv_size);

    return out;
}

uint8_t* HevcDecoder::get_frame_buffer(size_t size) {
    if (frame_pool_.empty()) {
        return nullptr;
    }

    // Round-robin through pool
    size_t idx = frame_pool_idx_++ % frame_pool_.size();

    if (frame_pool_[idx].size() < size) {
        frame_pool_[idx].resize(size);
    }

    return frame_pool_[idx].data();
}

}  // namespace av
}  // namespace atsc3
