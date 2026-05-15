// audio_decoder.cc — Audio Decoder implementation
//
// FFmpeg-based AC-4/AAC decoder for ATSC 3.0 audio

#include "audio_decoder.h"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// ATSC3_HAS_AC4_CODEC and ATSC3_HAS_MPEGH_CODEC are defined by CMake
// via feature detection (see av/CMakeLists.txt)

namespace atsc3 {
namespace av {

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() {
    shutdown();
}

bool AudioDecoder::init(const AudioDecoderConfig& config) {
    if (initialized_.load()) {
        return true;
    }

    config_ = config;

    if (!init_codec(config.codec)) {
        return false;
    }

    // Pre-allocate resample buffer
    size_t max_samples =
        AudioDecoderConfig::MAX_SAMPLES_PER_FRAME * AudioDecoderConfig::MAX_CHANNELS;
    resample_buffer_.resize(max_samples);

    initialized_.store(true);
    return true;
}

void AudioDecoder::shutdown() {
    if (!initialized_.load()) {
        return;
    }

    initialized_.store(false);
    cleanup_codec();
    resample_buffer_.clear();
    output_buffer_.clear();
}

bool AudioDecoder::decode(const uint8_t* data, size_t size, int64_t pts) {
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

    double n = static_cast<double>(stats_.frames_decoded);
    if (n > 0) {
        stats_.average_decode_time_ms = (stats_.average_decode_time_ms * (n - 1) + decode_ms) / n;
    }

    return true;
}

std::optional<AudioFrame> AudioDecoder::get_frame() {
    return output_buffer_.pop();
}

void AudioDecoder::set_frame_callback(AudioFrameCallback callback) {
    frame_callback_ = std::move(callback);
}

void AudioDecoder::flush() {
    if (!initialized_.load() || codec_ctx_ == nullptr) {
        return;
    }

    avcodec_send_packet(codec_ctx_, nullptr);
    receive_frames();
}

AudioDecoderStats AudioDecoder::get_stats() const {
    return stats_;
}

void AudioDecoder::reset_stats() {
    stats_ = AudioDecoderStats();
}

bool AudioDecoder::get_output_format(uint32_t& sample_rate, uint8_t& channels) const {
    if (!initialized_.load() || codec_ctx_ == nullptr) {
        return false;
    }

    sample_rate = static_cast<uint32_t>(codec_ctx_->sample_rate);

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
    channels = static_cast<uint8_t>(codec_ctx_->ch_layout.nb_channels);
#else
    channels = static_cast<uint8_t>(codec_ctx_->channels);
#endif

    return sample_rate > 0 && channels > 0;
}

bool AudioDecoder::init_codec(AudioCodecType codec) {
    AVCodecID codec_id;

    switch (codec) {
        case AudioCodecType::AC4:
#ifdef ATSC3_HAS_AC4_CODEC
            codec_id = AV_CODEC_ID_AC4;
            detected_codec_ = AudioCodecType::AC4;
#else
            // AC-4 not available, fall back to AAC
            codec_id = AV_CODEC_ID_AAC;
            detected_codec_ = AudioCodecType::AAC;
#endif
            break;
        case AudioCodecType::AAC:
            codec_id = AV_CODEC_ID_AAC;
            detected_codec_ = AudioCodecType::AAC;
            break;
        case AudioCodecType::MPEG_H:
#ifdef ATSC3_HAS_MPEGH_CODEC
            codec_id = AV_CODEC_ID_MPEGH_3D_AUDIO;
            detected_codec_ = AudioCodecType::MPEG_H;
#else
            // MPEG-H not available, fall back to AAC
            codec_id = AV_CODEC_ID_AAC;
            detected_codec_ = AudioCodecType::AAC;
#endif
            break;
        default:
            // Default to AAC
            codec_id = AV_CODEC_ID_AAC;
            detected_codec_ = AudioCodecType::AAC;
            break;
    }

    const AVCodec* av_codec = avcodec_find_decoder(codec_id);
    if (av_codec == nullptr) {
        // Fall back to AAC if requested codec not available
        if (codec_id != AV_CODEC_ID_AAC) {
            av_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
            detected_codec_ = AudioCodecType::AAC;
        }
        if (av_codec == nullptr) {
            return false;
        }
    }

    codec_ctx_ = avcodec_alloc_context3(av_codec);
    if (codec_ctx_ == nullptr) {
        return false;
    }

    // Request specific output format if configured
    if (config_.output_sample_rate > 0) {
        codec_ctx_->sample_rate = static_cast<int>(config_.output_sample_rate);
    }

    if (avcodec_open2(codec_ctx_, av_codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    decode_frame_ = av_frame_alloc();
    decode_packet_ = av_packet_alloc();

    if (decode_frame_ == nullptr || decode_packet_ == nullptr) {
        cleanup_codec();
        return false;
    }

    return true;
}

void AudioDecoder::cleanup_codec() {
    if (swr_ctx_ != nullptr) {
        swr_free(&swr_ctx_);
    }
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

bool AudioDecoder::init_resampler() {
    if (codec_ctx_ == nullptr) {
        return false;
    }

    // Clean up existing resampler
    if (swr_ctx_ != nullptr) {
        swr_free(&swr_ctx_);
    }

#if LIBSWRESAMPLE_VERSION_INT >= AV_VERSION_INT(4, 5, 100)
    // New channel layout API
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, codec_ctx_->ch_layout.nb_channels);

    swr_alloc_set_opts2(&swr_ctx_, &out_layout, AV_SAMPLE_FMT_FLT, codec_ctx_->sample_rate,
                        &codec_ctx_->ch_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate, 0,
                        nullptr);
#else
    // Legacy channel layout API
    int64_t out_layout = codec_ctx_->channel_layout;
    if (out_layout == 0) {
        out_layout = av_get_default_channel_layout(codec_ctx_->channels);
    }

    swr_ctx_ =
        swr_alloc_set_opts(nullptr, out_layout, AV_SAMPLE_FMT_FLT, codec_ctx_->sample_rate,
                           out_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate, 0, nullptr);
#endif

    if (swr_ctx_ == nullptr) {
        return false;
    }

    if (swr_init(swr_ctx_) < 0) {
        swr_free(&swr_ctx_);
        return false;
    }

    return true;
}

bool AudioDecoder::send_packet(const uint8_t* data, size_t size, int64_t pts) {
    decode_packet_->data = const_cast<uint8_t*>(data);
    decode_packet_->size = static_cast<int>(size);
    decode_packet_->pts = pts;
    decode_packet_->dts = pts;

    int ret = avcodec_send_packet(codec_ctx_, decode_packet_);

    decode_packet_->data = nullptr;
    decode_packet_->size = 0;

    return ret >= 0 || ret == AVERROR(EAGAIN);
}

bool AudioDecoder::receive_frames() {
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

        // Initialize resampler on first frame (now we know format)
        if (swr_ctx_ == nullptr) {
            if (!init_resampler()) {
                av_frame_unref(decode_frame_);
                continue;
            }
        }

        AudioFrame frame = convert_frame(decode_frame_);

        if (frame.valid()) {
            stats_.frames_decoded++;
            stats_.samples_decoded += frame.num_samples;

            if (!output_buffer_.push(std::move(frame))) {
                stats_.frames_dropped++;
            }

            if (frame_callback_) {
                const AudioFrame* queued = output_buffer_.peek();
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

AudioFrame AudioDecoder::convert_frame(AVFrame* frame) {
    AudioFrame out;

    if (frame == nullptr || frame->data[0] == nullptr) {
        return out;
    }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
    int channels = frame->ch_layout.nb_channels;
#else
    int channels = frame->channels;
#endif

    out.sample_rate = static_cast<uint32_t>(frame->sample_rate);
    out.channels = static_cast<uint8_t>(channels);
    out.num_samples = static_cast<uint32_t>(frame->nb_samples);
    out.pts = frame->pts;
    out.format = SampleFormat::FLOAT32;
    out.frame_number = frame_number_.fetch_add(1);

    if (out.sample_rate > 0) {
        out.duration_ms = static_cast<uint32_t>((static_cast<uint64_t>(out.num_samples) * 1000) /
                                                out.sample_rate);
    }

    // Allocate output buffer
    size_t total_samples = static_cast<size_t>(out.num_samples) * channels;
    out.samples.resize(total_samples);

    // Convert to float32
    if (swr_ctx_ != nullptr) {
        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(out.samples.data());
        int converted = swr_convert(swr_ctx_, &out_ptr, frame->nb_samples,
                                    const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        if (converted < 0) {
            return AudioFrame();
        }
        out.num_samples = static_cast<uint32_t>(converted);
    } else {
        // Direct copy if already float (shouldn't happen often)
        if (frame->format == AV_SAMPLE_FMT_FLT) {
            std::memcpy(out.samples.data(), frame->data[0], total_samples * sizeof(float));
        } else if (frame->format == AV_SAMPLE_FMT_FLTP) {
            // Interleave planar float
            float** planes = reinterpret_cast<float**>(frame->data);
            for (int s = 0; s < frame->nb_samples; s++) {
                for (int c = 0; c < channels; c++) {
                    out.samples[s * channels + c] = planes[c][s];
                }
            }
        }
    }

    return out;
}

}  // namespace av
}  // namespace atsc3
