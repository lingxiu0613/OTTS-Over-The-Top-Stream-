#include "otts/webrtc/audio_transcoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace otts::webrtc {
namespace {

std::string ffmpeg_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

AVSampleFormat first_sample_format(const AVCodec* codec, AVSampleFormat fallback) {
    if (codec != nullptr && codec->sample_fmts != nullptr && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
        return codec->sample_fmts[0];
    }
    return fallback;
}

bool copy_extradata(AVCodecContext* context, const std::vector<std::uint8_t>& data) {
    if (context == nullptr || data.empty()) {
        return false;
    }
    context->extradata = static_cast<std::uint8_t*>(
        av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (context->extradata == nullptr) {
        return false;
    }
    std::memcpy(context->extradata, data.data(), data.size());
    context->extradata_size = static_cast<int>(data.size());
    return true;
}

std::vector<std::uint8_t> opus_head() {
    return {
        'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
        1, 2,
        0, 0,
        0x80, 0xbb, 0, 0,
        0, 0,
        0};
}

}  // namespace

struct AudioTranscoder::Impl {
    AVCodecContext* decoder{nullptr};
    AVCodecContext* encoder{nullptr};
    SwrContext* resampler{nullptr};
    AVAudioFifo* fifo{nullptr};
    std::vector<std::uint8_t> output_config;
    std::string last_error;
    std::uint64_t encoded_samples{0};
    std::uint32_t timeline_origin_ms{0};
    bool timeline_started{false};

    ~Impl() {
        if (fifo != nullptr) {
            av_audio_fifo_free(fifo);
        }
        swr_free(&resampler);
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
    }

    bool open_decoder(AVCodecID codec_id, const std::vector<std::uint8_t>& extradata) {
        const auto* codec = avcodec_find_decoder(codec_id);
        if (codec == nullptr) {
            last_error = "decoder unavailable";
            return false;
        }
        decoder = avcodec_alloc_context3(codec);
        if (decoder == nullptr) {
            last_error = "failed to allocate decoder";
            return false;
        }
        decoder->pkt_timebase = AVRational{1, codec_id == AV_CODEC_ID_OPUS ? 48000 : 1000};
        if (codec_id == AV_CODEC_ID_OPUS) {
            decoder->sample_rate = 48000;
            av_channel_layout_default(&decoder->ch_layout, 2);
        }
        if (!extradata.empty() && !copy_extradata(decoder, extradata)) {
            last_error = "failed to allocate decoder extradata";
            return false;
        }
        const auto result = avcodec_open2(decoder, codec, nullptr);
        if (result < 0) {
            last_error = "failed to open decoder: " + ffmpeg_error(result);
            return false;
        }
        return true;
    }

    bool open_encoder(AVCodecID codec_id) {
        const AVCodec* codec = nullptr;
        if (codec_id == AV_CODEC_ID_OPUS) {
            codec = avcodec_find_encoder_by_name("libopus");
        }
        if (codec == nullptr) {
            codec = avcodec_find_encoder(codec_id);
        }
        if (codec == nullptr) {
            last_error = "encoder unavailable";
            return false;
        }
        encoder = avcodec_alloc_context3(codec);
        if (encoder == nullptr) {
            last_error = "failed to allocate encoder";
            return false;
        }
        encoder->sample_rate = 48000;
        encoder->sample_fmt = first_sample_format(
            codec,
            codec_id == AV_CODEC_ID_AAC ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT);
        encoder->bit_rate = 128000;
        encoder->time_base = AVRational{1, encoder->sample_rate};
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&encoder->ch_layout, 2);
        if (codec_id == AV_CODEC_ID_OPUS) {
            av_opt_set(encoder->priv_data, "application", "audio", 0);
            av_opt_set_int(encoder->priv_data, "frame_duration", 20, 0);
        }
        const auto result = avcodec_open2(encoder, codec, nullptr);
        if (result < 0) {
            last_error = "failed to open encoder: " + ffmpeg_error(result);
            return false;
        }
        fifo = av_audio_fifo_alloc(encoder->sample_fmt, encoder->ch_layout.nb_channels, 1);
        if (fifo == nullptr) {
            last_error = "failed to allocate audio FIFO";
            return false;
        }
        if (encoder->extradata != nullptr && encoder->extradata_size > 0) {
            output_config.assign(
                encoder->extradata,
                encoder->extradata + encoder->extradata_size);
        }
        return true;
    }

    bool ensure_resampler(const AVFrame* input) {
        if (resampler != nullptr) {
            return true;
        }
        AVChannelLayout input_layout{};
        if (input->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&input_layout, &input->ch_layout);
        } else if (decoder->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&input_layout, &decoder->ch_layout);
        } else {
            av_channel_layout_default(&input_layout, 2);
        }
        const auto input_rate = input->sample_rate > 0 ? input->sample_rate : decoder->sample_rate;
        const auto result = swr_alloc_set_opts2(
            &resampler,
            &encoder->ch_layout,
            encoder->sample_fmt,
            encoder->sample_rate,
            &input_layout,
            static_cast<AVSampleFormat>(input->format),
            input_rate,
            0,
            nullptr);
        av_channel_layout_uninit(&input_layout);
        if (result < 0 || resampler == nullptr) {
            last_error = "failed to configure audio resampler: " + ffmpeg_error(result);
            return false;
        }
        const auto init_result = swr_init(resampler);
        if (init_result < 0) {
            last_error = "failed to initialize audio resampler: " + ffmpeg_error(init_result);
            return false;
        }
        return true;
    }

    bool append_decoded_frame(const AVFrame* input) {
        if (!ensure_resampler(input)) {
            return false;
        }
        const auto input_rate = input->sample_rate > 0 ? input->sample_rate : decoder->sample_rate;
        const auto capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler, input_rate) + input->nb_samples,
            encoder->sample_rate,
            input_rate,
            AV_ROUND_UP));
        auto* converted = av_frame_alloc();
        if (converted == nullptr) {
            last_error = "failed to allocate converted audio frame";
            return false;
        }
        converted->format = encoder->sample_fmt;
        converted->sample_rate = encoder->sample_rate;
        converted->nb_samples = std::max(1, capacity);
        av_channel_layout_copy(&converted->ch_layout, &encoder->ch_layout);
        auto result = av_frame_get_buffer(converted, 0);
        if (result < 0) {
            last_error = "failed to allocate converted audio samples: " + ffmpeg_error(result);
            av_frame_free(&converted);
            return false;
        }
        const std::uint8_t** input_data =
            const_cast<const std::uint8_t**>(input->extended_data);
        result = swr_convert(
            resampler,
            converted->data,
            converted->nb_samples,
            input_data,
            input->nb_samples);
        if (result < 0) {
            last_error = "audio resampling failed: " + ffmpeg_error(result);
            av_frame_free(&converted);
            return false;
        }
        converted->nb_samples = result;
        if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + result) < 0 ||
            av_audio_fifo_write(fifo, reinterpret_cast<void**>(converted->data), result) < result) {
            last_error = "failed to append converted audio samples";
            av_frame_free(&converted);
            return false;
        }
        av_frame_free(&converted);
        return true;
    }

    std::vector<TranscodedAudioFrame> encode_available() {
        std::vector<TranscodedAudioFrame> output;
        const auto frame_size = encoder->frame_size > 0 ? encoder->frame_size : 960;
        while (av_audio_fifo_size(fifo) >= frame_size) {
            auto* frame = av_frame_alloc();
            if (frame == nullptr) {
                last_error = "failed to allocate encoder frame";
                break;
            }
            frame->format = encoder->sample_fmt;
            frame->sample_rate = encoder->sample_rate;
            frame->nb_samples = frame_size;
            frame->pts = static_cast<std::int64_t>(encoded_samples);
            av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
            auto result = av_frame_get_buffer(frame, 0);
            if (result < 0) {
                last_error = "failed to allocate encoder samples: " + ffmpeg_error(result);
                av_frame_free(&frame);
                break;
            }
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->data), frame_size) < frame_size) {
                last_error = "failed to read encoder samples";
                av_frame_free(&frame);
                break;
            }
            result = avcodec_send_frame(encoder, frame);
            encoded_samples += static_cast<std::uint64_t>(frame_size);
            av_frame_free(&frame);
            if (result < 0) {
                last_error = "failed to submit encoder frame: " + ffmpeg_error(result);
                break;
            }
            auto* packet = av_packet_alloc();
            if (packet == nullptr) {
                last_error = "failed to allocate encoded packet";
                break;
            }
            while ((result = avcodec_receive_packet(encoder, packet)) >= 0) {
                const auto packet_pts = packet->pts == AV_NOPTS_VALUE
                    ? static_cast<std::int64_t>(encoded_samples - frame_size)
                    : std::max<std::int64_t>(0, packet->pts);
                TranscodedAudioFrame encoded;
                encoded.timestamp_ms = timeline_origin_ms + static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(packet_pts) * 1000) / encoder->sample_rate);
                encoded.data.assign(packet->data, packet->data + packet->size);
                output.push_back(std::move(encoded));
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
                last_error = "failed to receive encoded packet: " + ffmpeg_error(result);
                break;
            }
        }
        return output;
    }

    std::vector<TranscodedAudioFrame> push(
        const std::uint8_t* data,
        std::size_t size,
        std::uint32_t timestamp_ms) {
        std::vector<TranscodedAudioFrame> output;
        if (data == nullptr || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return output;
        }
        if (!timeline_started) {
            timeline_origin_ms = timestamp_ms;
            timeline_started = true;
        }
        auto* packet = av_packet_alloc();
        if (packet == nullptr || av_new_packet(packet, static_cast<int>(size)) < 0) {
            last_error = "failed to allocate decoder packet";
            av_packet_free(&packet);
            return output;
        }
        std::memcpy(packet->data, data, size);
        auto result = avcodec_send_packet(decoder, packet);
        av_packet_free(&packet);
        if (result < 0) {
            last_error = "failed to submit decoder packet: " + ffmpeg_error(result);
            return output;
        }
        auto* frame = av_frame_alloc();
        if (frame == nullptr) {
            last_error = "failed to allocate decoder frame";
            return output;
        }
        while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (!append_decoded_frame(frame)) {
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
            auto encoded = encode_available();
            output.insert(
                output.end(),
                std::make_move_iterator(encoded.begin()),
                std::make_move_iterator(encoded.end()));
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF && last_error.empty()) {
            last_error = "failed to receive decoded audio: " + ffmpeg_error(result);
        }
        return output;
    }
};

AudioTranscoder::AudioTranscoder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

AudioTranscoder::~AudioTranscoder() = default;

std::unique_ptr<AudioTranscoder> AudioTranscoder::create_opus_to_aac(std::string& error) {
    auto impl = std::make_unique<Impl>();
    if (!impl->open_decoder(AV_CODEC_ID_OPUS, opus_head()) ||
        !impl->open_encoder(AV_CODEC_ID_AAC)) {
        error = impl->last_error;
        return nullptr;
    }
    return std::unique_ptr<AudioTranscoder>(new AudioTranscoder(std::move(impl)));
}

std::unique_ptr<AudioTranscoder> AudioTranscoder::create_aac_to_opus(
    const std::vector<std::uint8_t>& audio_specific_config,
    std::string& error) {
    auto impl = std::make_unique<Impl>();
    if (audio_specific_config.empty()) {
        error = "AAC AudioSpecificConfig is empty";
        return nullptr;
    }
    if (!impl->open_decoder(AV_CODEC_ID_AAC, audio_specific_config) ||
        !impl->open_encoder(AV_CODEC_ID_OPUS)) {
        error = impl->last_error;
        return nullptr;
    }
    return std::unique_ptr<AudioTranscoder>(new AudioTranscoder(std::move(impl)));
}

std::vector<TranscodedAudioFrame> AudioTranscoder::transcode(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t timestamp_ms) {
    return impl_->push(data, size, timestamp_ms);
}

const std::vector<std::uint8_t>& AudioTranscoder::output_codec_config() const {
    return impl_->output_config;
}

const std::string& AudioTranscoder::last_error() const {
    return impl_->last_error;
}

}  // namespace otts::webrtc
