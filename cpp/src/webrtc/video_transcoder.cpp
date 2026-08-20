#include "otts/webrtc/video_transcoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <array>

namespace otts::webrtc {
namespace {

std::string ffmpeg_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

}  // namespace

struct VideoTranscoder::Impl {
    AVCodecContext* decoder{nullptr};
    AVCodecContext* encoder{nullptr};
    std::string last_error;

    ~Impl() {
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
    }

    bool open_decoder() {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!decoder) {
            last_error = "HEVC decoder unavailable";
            return false;
        }
        decoder->pkt_timebase = AVRational{1, 1000};
        const auto result = avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            last_error = "HEVC decoder open failed: " + ffmpeg_error(result);
            return false;
        }
        return true;
    }

    bool ensure_encoder(const AVFrame* input) {
        if (encoder) return true;
        const auto* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        encoder = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!encoder) {
            last_error = "H.264 encoder unavailable";
            return false;
        }
        encoder->width = input->width;
        encoder->height = input->height;
        encoder->pix_fmt = static_cast<AVPixelFormat>(input->format);
        encoder->time_base = AVRational{1, 1000};
        encoder->framerate = AVRational{30, 1};
        encoder->bit_rate = 2500000;
        encoder->gop_size = 60;
        encoder->max_b_frames = 0;
        av_opt_set(encoder->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
        av_opt_set(encoder->priv_data, "x264-params", "repeat-headers=1:annexb=1:keyint=60", 0);
        const auto result = avcodec_open2(encoder, codec, nullptr);
        if (result < 0) {
            last_error = "H.264 encoder open failed: " + ffmpeg_error(result);
            return false;
        }
        return true;
    }

    std::vector<TranscodedVideoFrame> transcode(const std::uint8_t* data, std::size_t size, std::uint32_t timestamp_ms) {
        std::vector<TranscodedVideoFrame> output;
        auto* packet = av_packet_alloc();
        auto* frame = av_frame_alloc();
        if (!packet || !frame) {
            last_error = "video frame allocation failed";
            av_packet_free(&packet);
            av_frame_free(&frame);
            return output;
        }
        packet->data = const_cast<std::uint8_t*>(data);
        packet->size = static_cast<int>(size);
        packet->pts = timestamp_ms;
        packet->dts = timestamp_ms;
        auto result = avcodec_send_packet(decoder, packet);
        packet->data = nullptr;
        packet->size = 0;
        if (result < 0) last_error = "HEVC decode submit failed: " + ffmpeg_error(result);
        while (result >= 0 && (result = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (!ensure_encoder(frame)) break;
            frame->pts = timestamp_ms;
            // Decoder picture types describe the HEVC coding structure. The
            // low-latency AVC encoder has B-frames disabled and must choose a
            // fresh AVC picture type instead of inheriting HEVC B-frame flags.
            frame->pict_type = AV_PICTURE_TYPE_NONE;
            auto encode_result = avcodec_send_frame(encoder, frame);
            if (encode_result < 0) {
                last_error = "H.264 encode submit failed: " + ffmpeg_error(encode_result);
                break;
            }
            while ((encode_result = avcodec_receive_packet(encoder, packet)) >= 0) {
                TranscodedVideoFrame encoded;
                encoded.annexb.assign(packet->data, packet->data + packet->size);
                encoded.timestamp_ms = packet->pts >= 0 ? static_cast<std::uint32_t>(packet->pts) : timestamp_ms;
                encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                output.push_back(std::move(encoded));
                av_packet_unref(packet);
            }
            av_frame_unref(frame);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
        return output;
    }
};

VideoTranscoder::VideoTranscoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoTranscoder::~VideoTranscoder() = default;

std::unique_ptr<VideoTranscoder> VideoTranscoder::create_hevc_to_avc(std::string& error) {
    auto impl = std::make_unique<Impl>();
    if (!impl->open_decoder()) {
        error = impl->last_error;
        return nullptr;
    }
    return std::unique_ptr<VideoTranscoder>(new VideoTranscoder(std::move(impl)));
}

std::vector<TranscodedVideoFrame> VideoTranscoder::transcode(
    const std::uint8_t* data, std::size_t size, std::uint32_t timestamp_ms) {
    return impl_->transcode(data, size, timestamp_ms);
}

const std::string& VideoTranscoder::last_error() const { return impl_->last_error; }

}  // namespace otts::webrtc
