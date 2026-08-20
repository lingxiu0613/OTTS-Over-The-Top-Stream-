#include "otts/webrtc/webrtc_service.hpp"

#include "otts/codec/video_codec.hpp"
#include "otts/core/logger.hpp"
#include "otts/rtmp/stream_registry.hpp"
#include "otts/webrtc/audio_transcoder.hpp"
#include "otts/webrtc/video_transcoder.hpp"

#include <algorithm>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <atomic>
#include <cctype>
#include <functional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#if OTTS_WEBRTC_DATACHANNEL
#include <rtc/rtc.hpp>
#endif

namespace otts::webrtc {

namespace {

std::string direction_to_string(SessionDirection direction) {
    return direction == SessionDirection::Publish ? "whip" : "whep";
}

std::string state_to_string(SessionState state) {
    switch (state) {
        case SessionState::Pending:
            return "pending";
        case SessionState::AwaitingTransport:
            return "awaiting_transport";
        case SessionState::Connected:
            return "connected";
        case SessionState::Closed:
            return "closed";
        case SessionState::Failed:
            return "failed";
    }
    return "unknown";
}

struct WebRtcPayloadTypes {
    std::uint8_t h264{102};
    std::uint8_t h265{104};
    std::uint8_t opus{111};
    bool has_h264{false};
    bool has_h265{false};
    otts::media::CodecId preferred_video_codec{otts::media::CodecId::Unknown};
    std::string video_mid{"video"};
    std::string audio_mid{"audio"};
};

std::string uppercase_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

WebRtcPayloadTypes parse_offer_payload_types(const std::string& offer_sdp) {
    WebRtcPayloadTypes types;
    std::unordered_map<int, std::string> codecs;
    std::unordered_map<int, std::string> fmtps;
    std::vector<int> video_payload_order;
    std::istringstream input(offer_sdp);
    std::string line;
    std::string current_media;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("m=", 0) == 0) {
            if (line.rfind("m=video ", 0) == 0) {
                current_media = "video";
                std::istringstream media(line);
                std::string token;
                for (int field = 0; media >> token; ++field) {
                    if (field < 3) continue;
                    try {
                        video_payload_order.push_back(std::stoi(token));
                    } catch (...) {
                    }
                }
            } else if (line.rfind("m=audio ", 0) == 0) {
                current_media = "audio";
            } else {
                current_media.clear();
            }
        } else if (line.rfind("a=mid:", 0) == 0) {
            const auto mid = line.substr(std::strlen("a=mid:"));
            if (current_media == "video" && !mid.empty()) {
                types.video_mid = mid;
            } else if (current_media == "audio" && !mid.empty()) {
                types.audio_mid = mid;
            }
        } else if (line.rfind("a=rtpmap:", 0) == 0) {
            const auto space = line.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            try {
                const auto pt = std::stoi(line.substr(std::strlen("a=rtpmap:"), space - std::strlen("a=rtpmap:")));
                auto codec = line.substr(space + 1);
                const auto slash = codec.find('/');
                if (slash != std::string::npos) {
                    codec = codec.substr(0, slash);
                }
                codecs[pt] = uppercase_copy(codec);
            } catch (...) {
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            const auto space = line.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            try {
                const auto pt = std::stoi(line.substr(std::strlen("a=fmtp:"), space - std::strlen("a=fmtp:")));
                fmtps[pt] = line.substr(space + 1);
            } catch (...) {
            }
        }
    }

    bool found_h264 = false;
    for (const auto& [pt, codec] : codecs) {
        if (codec == "OPUS" && pt >= 0 && pt <= 255) {
            types.opus = static_cast<std::uint8_t>(pt);
        } else if (codec == "H264" && pt >= 0 && pt <= 255) {
            const auto fmtp = fmtps.find(pt);
            const auto supports_packetization_mode_one =
                fmtp == fmtps.end() || fmtp->second.find("packetization-mode=1") != std::string::npos;
            if (!found_h264 || (supports_packetization_mode_one && fmtp != fmtps.end() &&
                                fmtp->second.find("profile-level-id=42e01f") != std::string::npos)) {
                types.h264 = static_cast<std::uint8_t>(pt);
                types.has_h264 = true;
                found_h264 = true;
            }
        } else if ((codec == "H265" || codec == "HEVC") && pt >= 0 && pt <= 255) {
            types.h265 = static_cast<std::uint8_t>(pt);
            types.has_h265 = true;
        }
    }
    for (const auto pt : video_payload_order) {
        const auto codec = codecs.find(pt);
        if (codec == codecs.end()) continue;
        if ((codec->second == "H265" || codec->second == "HEVC") && types.has_h265) {
            types.preferred_video_codec = otts::media::CodecId::Hevc;
            break;
        }
        if (codec->second == "H264" && types.has_h264) {
            types.preferred_video_codec = otts::media::CodecId::Avc;
            break;
        }
    }
    return types;
}

std::vector<std::uint8_t> bytes_from_rtc_binary(const std::vector<std::byte>& data) {
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    for (const auto byte : data) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

std::vector<std::byte> rtc_binary_from_bytes(
    const std::vector<std::uint8_t>& data,
    std::size_t offset = 0) {
    std::vector<std::byte> out;
    if (offset >= data.size()) {
        return out;
    }
    out.reserve(data.size() - offset);
    for (std::size_t i = offset; i < data.size(); ++i) {
        out.push_back(static_cast<std::byte>(data[i]));
    }
    return out;
}

otts::rtmp::MediaMessage make_aac_audio_message(
    std::uint32_t timestamp,
    std::uint8_t packet_type,
    const std::vector<std::uint8_t>& aac) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 8;
    message.message_stream_id = 1;
    message.payload.reserve(aac.size() + 2);
    message.payload.push_back(0xaf);
    message.payload.push_back(packet_type);
    message.payload.insert(message.payload.end(), aac.begin(), aac.end());
    return message;
}

std::vector<std::uint8_t> opus_audio_payload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 2 || ((payload[0] >> 4) & 0x0f) != 13) {
        return {};
    }
    return std::vector<std::uint8_t>(payload.begin() + 1, payload.end());
}

std::vector<std::uint8_t> aac_audio_payload(
    const std::vector<std::uint8_t>& payload,
    std::uint8_t packet_type) {
    if (payload.size() < 3 || ((payload[0] >> 4) & 0x0f) != 10 || payload[1] != packet_type) {
        return {};
    }
    return std::vector<std::uint8_t>(payload.begin() + 2, payload.end());
}

#if OTTS_WEBRTC_DATACHANNEL
std::uint32_t media_timestamp_ms_from_rtp(
    rtc::FrameInfo info,
    std::shared_ptr<std::optional<std::uint32_t>> base_ts,
    std::uint32_t clock_rate) {
    std::uint32_t timestamp = info.timestamp;
    if (timestamp == 0 && info.timestampSeconds.has_value()) {
        timestamp = static_cast<std::uint32_t>(info.timestampSeconds->count() * static_cast<double>(clock_rate));
    }
    if (!base_ts->has_value()) {
        *base_ts = timestamp;
    }
    const auto delta = static_cast<std::uint32_t>(timestamp - **base_ts);
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(delta) * 1000ull) / static_cast<std::uint64_t>(clock_rate));
}
#endif

std::string rtc_state_to_transport_string(
#if OTTS_WEBRTC_DATACHANNEL
    rtc::PeerConnection::State state
#else
    int state
#endif
) {
#if OTTS_WEBRTC_DATACHANNEL
    switch (state) {
        case rtc::PeerConnection::State::New:
            return "new";
        case rtc::PeerConnection::State::Connecting:
            return "connecting";
        case rtc::PeerConnection::State::Connected:
            return "connected";
        case rtc::PeerConnection::State::Disconnected:
            return "disconnected";
        case rtc::PeerConnection::State::Failed:
            return "failed";
        case rtc::PeerConnection::State::Closed:
            return "closed";
    }
#else
    (void)state;
#endif
    return "unknown";
}

SessionState rtc_state_to_session_state(
#if OTTS_WEBRTC_DATACHANNEL
    rtc::PeerConnection::State state
#else
    int state
#endif
) {
#if OTTS_WEBRTC_DATACHANNEL
    switch (state) {
        case rtc::PeerConnection::State::Connected:
            return SessionState::Connected;
        case rtc::PeerConnection::State::Failed:
            return SessionState::Failed;
        case rtc::PeerConnection::State::Closed:
            return SessionState::Closed;
        case rtc::PeerConnection::State::New:
        case rtc::PeerConnection::State::Connecting:
        case rtc::PeerConnection::State::Disconnected:
            return SessionState::AwaitingTransport;
    }
#else
    (void)state;
#endif
    return SessionState::AwaitingTransport;
}

void upsert_native_protocol_session(
    otts::rtmp::StreamRegistry* registry,
    const std::string& session_id,
    const std::string& stream_key,
    SessionDirection direction,
    const std::string& state,
    const std::string& native_stage,
    const std::string& codec_hint,
    std::uint64_t started_at_epoch_ms,
    const std::string& last_error = {}) {
    if (registry == nullptr) {
        return;
    }
    registry->upsert_external_session(
        "cpp-webrtc:" + session_id,
        stream_key,
        otts::media::StreamSource::Whip,
        direction == SessionDirection::Publish ? "publish" : "play",
        "cpp-webrtc-native",
        state,
        "",
        "",
        "",
        "webrtc/ice-dtls-srtp",
        direction == SessionDirection::Publish ? "native-whip-video-ingress" : "native-whep-video-egress",
        native_stage,
        codec_hint,
        0,
        started_at_epoch_ms,
        0,
        0,
        0,
        last_error);
}

}  // namespace

struct WebRtcService::NativeSession {
    std::string session_id;
    std::string stream_key;
    SessionDirection direction{SessionDirection::Play};
    otts::rtmp::StreamRegistry* registry{nullptr};
    otts::rtmp::StreamRegistry::CallbackId callback_id{0};
    std::atomic<bool> open{false};
    std::atomic<bool> video_track_open{false};
    std::atomic<bool> audio_track_open{false};
    std::atomic<bool> play_callback_registered{false};
    std::atomic<bool> video_keyframe_seen{false};
    std::atomic<std::uint64_t> video_frames{0};
    std::atomic<std::uint64_t> video_bytes{0};
    std::atomic<std::uint64_t> audio_frames{0};
    std::atomic<std::uint64_t> audio_bytes{0};
    std::mutex cleanup_mutex;
    std::mutex media_mutex;
    std::mutex audio_codec_mutex;
    std::vector<std::uint8_t> video_config_sample;
    otts::media::CodecId video_codec{otts::media::CodecId::Avc};
    otts::media::CodecId source_video_codec{otts::media::CodecId::Avc};
    std::uint64_t started_at_epoch_ms{0};
#if OTTS_WEBRTC_DATACHANNEL
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter;
    std::shared_ptr<rtc::RtcpSrReporter> audio_sr_reporter;
    std::unique_ptr<AudioTranscoder> opus_to_aac;
    std::unique_ptr<AudioTranscoder> aac_to_opus;
    std::unique_ptr<VideoTranscoder> hevc_to_avc;
    std::vector<std::uint8_t> aac_config;
    bool aac_sequence_published{false};
    bool audio_transcode_error_logged{false};
    std::mutex outbound_mutex;
    std::condition_variable outbound_cv;
    std::deque<otts::rtmp::MediaMessage> outbound_queue;
    std::thread outbound_thread;
    bool outbound_stop{false};
    bool outbound_started{false};
    bool outbound_waiting_for_keyframe{false};
    std::uint64_t outbound_overflows{0};

    void send_audio_to_webrtc(const otts::rtmp::MediaMessage& message, const std::string& context) {
        auto config = aac_audio_payload(message.payload, 0);
        if (!config.empty()) {
            std::lock_guard<std::mutex> lock(audio_codec_mutex);
            if (config != aac_config || !aac_to_opus) {
                std::string error;
                auto transcoder = AudioTranscoder::create_aac_to_opus(config, error);
                if (!transcoder) {
                    otts::core::log_warn("webrtc_native", context + " AAC decoder init failed: " + error);
                    return;
                }
                aac_config = std::move(config);
                aac_to_opus = std::move(transcoder);
                otts::core::log_info("webrtc_native", context + " AAC-to-Opus transcoder ready key=" + stream_key);
            }
            return;
        }

        if (!audio_track || !audio_track_open.load()) {
            return;
        }

        auto opus = opus_audio_payload(message.payload);
        if (!opus.empty()) {
            rtc::FrameInfo info(std::chrono::duration<double, std::milli>(message.timestamp));
            try {
                audio_track->sendFrame(rtc_binary_from_bytes(opus), info);
                audio_frames.fetch_add(1);
                audio_bytes.fetch_add(opus.size());
            } catch (const std::exception& exc) {
                const std::string error = exc.what();
                otts::core::log_warn("webrtc_native", context + " Opus sendFrame failed: " + error);
                if (error.find("closed") != std::string::npos) {
                    detach_play_subscription();
                }
            }
            return;
        }

        auto aac = aac_audio_payload(message.payload, 1);
        if (aac.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(audio_codec_mutex);
        if (!aac_to_opus) {
            return;
        }
        auto frames = aac_to_opus->transcode(aac.data(), aac.size(), message.timestamp);
        for (const auto& frame : frames) {
            rtc::FrameInfo info(std::chrono::duration<double, std::milli>(frame.timestamp_ms));
            try {
                audio_track->sendFrame(rtc_binary_from_bytes(frame.data), info);
                audio_frames.fetch_add(1);
                audio_bytes.fetch_add(frame.data.size());
            } catch (const std::exception& exc) {
                const std::string error = exc.what();
                otts::core::log_warn("webrtc_native", context + " AAC-to-Opus sendFrame failed: " + error);
                if (error.find("closed") != std::string::npos) {
                    detach_play_subscription();
                }
                break;
            }
        }
    }

    void publish_opus_as_aac(const std::vector<std::uint8_t>& opus, std::uint32_t timestamp) {
        if (registry == nullptr || opus.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(audio_codec_mutex);
        if (!opus_to_aac) {
            std::string error;
            opus_to_aac = AudioTranscoder::create_opus_to_aac(error);
            if (!opus_to_aac) {
                otts::core::log_warn("webrtc_native", "WHIP Opus-to-AAC init failed: " + error);
                return;
            }
        }
        if (!aac_sequence_published) {
            const auto& config = opus_to_aac->output_codec_config();
            if (config.empty()) {
                otts::core::log_warn("webrtc_native", "WHIP AAC encoder returned empty AudioSpecificConfig");
                return;
            }
            registry->publish_external_media(
                stream_key,
                otts::media::StreamSource::Whip,
                "cpp-webrtc-native",
                make_aac_audio_message(timestamp, 0, config));
            aac_sequence_published = true;
            otts::core::log_info("webrtc_native", "WHIP Opus-to-AAC transcoder ready key=" + stream_key);
        }
        auto frames = opus_to_aac->transcode(opus.data(), opus.size(), timestamp);
        if (frames.empty() && !opus_to_aac->last_error().empty() && !audio_transcode_error_logged) {
            audio_transcode_error_logged = true;
            otts::core::log_warn(
                "webrtc_native",
                "WHIP Opus-to-AAC transcode failed key=" + stream_key +
                    " error=" + opus_to_aac->last_error());
        }
        for (const auto& frame : frames) {
            registry->publish_external_media(
                stream_key,
                otts::media::StreamSource::Whip,
                "cpp-webrtc-native",
                make_aac_audio_message(frame.timestamp_ms, 1, frame.data));
        }
    }

    void send_video_to_webrtc(const otts::rtmp::MediaMessage& message, const std::string& context) {
        if (message.type_id != 9) {
            return;
        }
        const auto packet = otts::codec::parse_flv_video_packet(message.payload);
        if (!packet || packet->codec != source_video_codec) return;
        std::vector<std::uint8_t> sample;
        bool is_video_keyframe = false;
        if (packet->sequence_header) {
            sample = otts::codec::flv_video_config_to_annexb(message.payload);
            if (!sample.empty()) {
                std::lock_guard<std::mutex> lock(media_mutex);
                video_config_sample = sample;
            }
            return;
        } else if (packet->coded_frames) {
            sample = otts::codec::flv_video_sample_to_annexb(message.payload);
            is_video_keyframe = packet->keyframe || otts::codec::is_keyframe(sample, video_codec);
        } else {
            return;
        }
        if (!video_track || !video_track_open.load()) {
            return;
        }
        if (source_video_codec == otts::media::CodecId::Hevc && video_codec == otts::media::CodecId::Avc) {
            if (!is_video_keyframe && !video_keyframe_seen.load()) {
                return;
            }
            if (is_video_keyframe) {
                std::lock_guard<std::mutex> lock(media_mutex);
                if (!video_config_sample.empty()) {
                    auto configured = video_config_sample;
                    configured.insert(configured.end(), sample.begin(), sample.end());
                    sample = std::move(configured);
                }
                video_keyframe_seen.store(true);
            }
            if (!hevc_to_avc) {
                std::string error;
                hevc_to_avc = VideoTranscoder::create_hevc_to_avc(error);
                if (!hevc_to_avc) {
                    otts::core::log_warn("webrtc_native", context + " HEVC-to-AVC init failed: " + error);
                    return;
                }
                otts::core::log_info("webrtc_native", context + " HEVC-to-AVC fallback ready key=" + stream_key);
            }
            const auto frames = hevc_to_avc->transcode(sample.data(), sample.size(), message.timestamp);
            for (const auto& frame : frames) {
                rtc::FrameInfo info(std::chrono::duration<double, std::milli>(frame.timestamp_ms));
                info.isKeyFrame = frame.keyframe;
                video_track->sendFrame(rtc_binary_from_bytes(frame.annexb), info);
                video_frames.fetch_add(1);
                video_bytes.fetch_add(frame.annexb.size());
            }
            return;
        }
        if (!is_video_keyframe && !video_keyframe_seen.load()) {
            return;
        }
        if (is_video_keyframe) {
            std::vector<std::uint8_t> with_config;
            {
                std::lock_guard<std::mutex> lock(media_mutex);
                with_config = video_config_sample;
            }
            if (!with_config.empty()) {
                with_config.insert(with_config.end(), sample.begin(), sample.end());
                sample = std::move(with_config);
            }
            video_keyframe_seen.store(true);
        }
        if (sample.empty()) {
            return;
        }
        rtc::FrameInfo info(std::chrono::duration<double, std::milli>(
            static_cast<std::int64_t>(message.timestamp) + packet->composition_time_ms));
        info.isKeyFrame = is_video_keyframe;
        try {
            video_track->sendFrame(rtc_binary_from_bytes(sample), info);
            video_frames.fetch_add(1);
            video_bytes.fetch_add(sample.size());
        } catch (const std::exception& exc) {
            const std::string error = exc.what();
            otts::core::log_warn("webrtc_native", context + " video sendFrame failed: " + error);
            if (error.find("closed") != std::string::npos) {
                detach_play_subscription();
            }
        }
    }

    void start_play_sender(const std::string& context) {
        std::lock_guard<std::mutex> lock(outbound_mutex);
        if (outbound_started) {
            return;
        }
        outbound_started = true;
        outbound_stop = false;
        outbound_thread = std::thread([this, context]() {
            bool clock_started = false;
            std::uint32_t media_origin = 0;
            auto wall_origin = std::chrono::steady_clock::now();
            while (true) {
                otts::rtmp::MediaMessage message;
                {
                    std::unique_lock<std::mutex> lock(outbound_mutex);
                    outbound_cv.wait(lock, [&]() { return outbound_stop || !outbound_queue.empty(); });
                    if (outbound_stop && outbound_queue.empty()) {
                        break;
                    }
                    message = std::move(outbound_queue.front());
                    outbound_queue.pop_front();
                }

                const auto now = std::chrono::steady_clock::now();
                if (!clock_started) {
                    media_origin = message.timestamp;
                    wall_origin = now;
                    clock_started = true;
                }
                auto delta_ms = static_cast<std::uint32_t>(message.timestamp - media_origin);
                if (delta_ms > 600000) {
                    media_origin = message.timestamp;
                    wall_origin = now;
                    delta_ms = 0;
                }
                auto target = wall_origin + std::chrono::milliseconds(delta_ms);
                if (target > now + std::chrono::milliseconds(500)) {
                    media_origin = message.timestamp;
                    wall_origin = now;
                    delta_ms = 0;
                    target = now;
                }
                if (now > target + std::chrono::milliseconds(500)) {
                    wall_origin = now - std::chrono::milliseconds(delta_ms);
                    target = now;
                }
                if (target > now) {
                    std::unique_lock<std::mutex> lock(outbound_mutex);
                    outbound_cv.wait_until(lock, target, [&]() { return outbound_stop; });
                    if (outbound_stop) {
                        break;
                    }
                }
                if (message.type_id == 8) {
                    send_audio_to_webrtc(message, context);
                } else if (message.type_id == 9) {
                    send_video_to_webrtc(message, context);
                }
            }
        });
    }

    void enqueue_play_media(const otts::rtmp::MediaMessage& message) {
        if (message.type_id != 8 && message.type_id != 9) {
            return;
        }
        std::lock_guard<std::mutex> lock(outbound_mutex);
        if (outbound_stop) {
            return;
        }
        // A late subscriber receives up to 512 GOP packets plus both sequence
        // headers. Preserve that complete startup snapshot so the decoder never
        // starts without SPS/PPS or its reference keyframe.
        if (outbound_queue.size() >= 1024) {
            outbound_queue.clear();
            outbound_waiting_for_keyframe = true;
            video_keyframe_seen.store(false);
            ++outbound_overflows;
            otts::core::log_warn(
                "webrtc_native",
                "WHEP outbound queue overflow; waiting for next keyframe key=" + stream_key +
                    " count=" + std::to_string(outbound_overflows));
        }

        if (outbound_waiting_for_keyframe && message.type_id == 9) {
            const auto packet = otts::codec::parse_flv_video_packet(message.payload);
            const bool sequence_header = packet && packet->sequence_header && packet->codec == video_codec;
            const bool keyframe = packet && packet->coded_frames && packet->keyframe && packet->codec == video_codec;
            if (!sequence_header && !keyframe) {
                return;
            }
            if (keyframe) {
                outbound_waiting_for_keyframe = false;
            }
        }
        outbound_queue.push_back(message);
        outbound_cv.notify_one();
    }

    void stop_play_sender() {
        {
            std::lock_guard<std::mutex> lock(outbound_mutex);
            outbound_stop = true;
            outbound_queue.clear();
        }
        outbound_cv.notify_all();
        if (outbound_thread.joinable() && outbound_thread.get_id() != std::this_thread::get_id()) {
            outbound_thread.join();
        }
        outbound_started = false;
    }
#endif
    void detach_play_subscription() {
        if (registry != nullptr && callback_id != 0 && direction == SessionDirection::Play) {
            registry->remove_callback_subscriber(stream_key, callback_id);
            registry->update_external_viewers(stream_key, otts::media::StreamSource::Whip, "cpp-webrtc-native", 0);
            callback_id = 0;
        }
        open.store(false);
        video_track_open.store(false);
        audio_track_open.store(false);
        play_callback_registered.store(false);
    }

    void close_now() {
        std::lock_guard<std::mutex> lock(cleanup_mutex);
        detach_play_subscription();
#if OTTS_WEBRTC_DATACHANNEL
        stop_play_sender();
#endif
        if (registry != nullptr) {
            registry->remove_external_session("cpp-webrtc:" + session_id);
            if (direction == SessionDirection::Publish) {
                registry->remove_external_stream(stream_key, otts::media::StreamSource::Whip);
            }
            registry = nullptr;
        }
#if OTTS_WEBRTC_DATACHANNEL
        video_track.reset();
        audio_track.reset();
        pc.reset();
#endif
    }

    ~NativeSession() {
        close_now();
    }
};

WebRtcService::WebRtcService(NativeStatus native_status)
    : native_status_(std::move(native_status)) {
    native_status_.compiled_with_dependency = (OTTS_WEBRTC_NATIVE_DEPENDENCY != 0) || (OTTS_WEBRTC_DATACHANNEL != 0);
#if OTTS_WEBRTC_DATACHANNEL
    native_status_.peer_factory_ready = true;
    native_status_.media_engine_ready = true;
    if (native_status_.detail.empty()) {
        native_status_.detail = "native WHIP/WHEP ready: H.264/H.265 + Opus wire, AAC core audio, HEVC-to-AVC fallback";
    }
#elif OTTS_WEBRTC_NATIVE_DEPENDENCY
    native_status_.peer_factory_ready = false;
#else
    native_status_.peer_factory_ready = false;
#endif
    if (native_status_.selected_runtime.empty()) {
        if (native_status_.configured_mode == RuntimeMode::Native) {
            native_status_.selected_runtime = "native";
        } else if (native_status_.configured_mode == RuntimeMode::Auto && native_status_.media_engine_ready) {
            native_status_.selected_runtime = "native";
        } else {
            native_status_.selected_runtime = "gateway";
        }
    }
    if (native_status_.detail.empty()) {
        native_status_.detail = native_status_.media_engine_ready
            ? "native WebRTC ready: H.264/H.265 + Opus wire, AAC core audio, HEVC-to-AVC fallback"
            : "native dependency hook is present; PeerConnection media engine is not wired yet";
    }
}

WebRtcService::~WebRtcService() = default;

void WebRtcService::attach_registry(otts::rtmp::StreamRegistry& registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_ = &registry;
}

std::string WebRtcService::create_session(
    SessionDirection direction,
    const std::string& stream_key,
    const std::string& offer_sdp) {
    SessionStateData state;
    state.session_id = make_session_id();
    state.stream_key = stream_key;
    state.direction = direction;
    state.state = SessionState::AwaitingTransport;
    state.offer_sdp = offer_sdp;
    state.answer_sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=OTTS WebRTC Placeholder\r\n"
        "t=0 0\r\n"
        "a=inactive\r\n";
    state.created_at_epoch_ms = now_epoch_ms();
    state.updated_at_epoch_ms = state.created_at_epoch_ms;

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[state.session_id] = state;
    return state.session_id;
}

NativeOfferResult WebRtcService::handle_native_offer(
    SessionDirection direction,
    const std::string& stream_key,
    const std::string& offer_sdp) {
    return handle_native_offer_locked(direction, stream_key, offer_sdp);
}

std::vector<SessionSnapshot> WebRtcService::snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSnapshot> result;
    result.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        std::uint64_t video_frames = 0;
        std::uint64_t video_bytes = 0;
        std::uint64_t audio_frames = 0;
        std::uint64_t audio_bytes = 0;
        if (const auto native_it = native_sessions_.find(session.session_id); native_it != native_sessions_.end()) {
            video_frames = native_it->second->video_frames.load();
            video_bytes = native_it->second->video_bytes.load();
            audio_frames = native_it->second->audio_frames.load();
            audio_bytes = native_it->second->audio_bytes.load();
        }
        result.push_back(make_snapshot(session, video_frames, video_bytes, audio_frames, audio_bytes));
    }
    return result;
}

std::optional<SessionSnapshot> WebRtcService::snapshot(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    std::uint64_t video_frames = 0;
    std::uint64_t video_bytes = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t audio_bytes = 0;
    if (const auto native_it = native_sessions_.find(session_id); native_it != native_sessions_.end()) {
        video_frames = native_it->second->video_frames.load();
        video_bytes = native_it->second->video_bytes.load();
        audio_frames = native_it->second->audio_frames.load();
        audio_bytes = native_it->second->audio_bytes.load();
    }
    return make_snapshot(it->second, video_frames, video_bytes, audio_frames, audio_bytes);
}

bool WebRtcService::close_session(const std::string& session_id) {
    std::shared_ptr<NativeSession> native_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return false;
        }
        if (const auto native_it = native_sessions_.find(session_id); native_it != native_sessions_.end()) {
            native_session = native_it->second;
            native_sessions_.erase(native_it);
        }
        sessions_.erase(it);
    }
    if (native_session) {
        native_session->close_now();
    }
    return true;
}

bool WebRtcService::fail_session(const std::string& session_id, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.state = SessionState::Failed;
    it->second.transport_state = "failed";
    it->second.last_error = error;
    it->second.updated_at_epoch_ms = now_epoch_ms();
    return true;
}

std::size_t WebRtcService::cleanup_stale_sessions(std::uint64_t terminal_session_retention_ms) {
    if (terminal_session_retention_ms == 0) {
        return 0;
    }

    std::vector<std::shared_ptr<NativeSession>> native_to_close;
    std::vector<std::pair<otts::rtmp::StreamRegistry*, std::string>> registry_sessions;
    std::size_t removed = 0;
    const auto now_ms = now_epoch_ms();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const auto terminal = it->second.state == SessionState::Closed || it->second.state == SessionState::Failed;
            const auto expired = terminal && now_ms > it->second.updated_at_epoch_ms &&
                                 now_ms - it->second.updated_at_epoch_ms > terminal_session_retention_ms;
            if (!expired) {
                ++it;
                continue;
            }

            if (const auto native_it = native_sessions_.find(it->first); native_it != native_sessions_.end()) {
                native_to_close.push_back(native_it->second);
                if (native_it->second->registry) {
                    registry_sessions.emplace_back(native_it->second->registry, native_it->second->session_id);
                }
                native_sessions_.erase(native_it);
            }
            it = sessions_.erase(it);
            removed += 1;
        }
    }

    for (const auto& native_session : native_to_close) {
        native_session->close_now();
    }
    for (const auto& [registry, session_id] : registry_sessions) {
        registry->remove_external_session("cpp-webrtc:" + session_id);
    }
    if (removed > 0) {
        otts::core::log_info("webrtc_service", "cleanup removed_terminal_sessions=" + std::to_string(removed));
    }
    return removed;
}

void WebRtcService::update_session_state(
    const std::string& session_id,
    SessionState state,
    const std::string& transport_state,
    const std::string& error) {
    std::shared_ptr<NativeSession> native_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return;
        }
        it->second.state = state;
        it->second.transport_state = transport_state;
        it->second.updated_at_epoch_ms = now_epoch_ms();
        if (!error.empty()) {
            it->second.last_error = error;
        }
        if (const auto native_it = native_sessions_.find(session_id); native_it != native_sessions_.end()) {
            native_session = native_it->second;
        }
    }
    if (native_session) {
        if (state == SessionState::Closed || state == SessionState::Failed) {
            native_session->detach_play_subscription();
        }
        upsert_native_protocol_session(
            native_session->registry,
            native_session->session_id,
            native_session->stream_key,
            native_session->direction,
            state_to_string(state),
            "native-cxx-libdatachannel:" + transport_state,
            "h264/h265+aac(core)/opus(webrtc)",
            native_session->started_at_epoch_ms,
            error);
    }
}

NativeStatus WebRtcService::native_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return native_status_;
}

bool WebRtcService::should_use_gateway() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (native_status_.configured_mode == RuntimeMode::Gateway) {
        return true;
    }
    if (native_status_.configured_mode == RuntimeMode::Auto) {
        return !native_status_.media_engine_ready;
    }
    return false;
}

bool WebRtcService::requires_native_http_answer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return native_status_.configured_mode == RuntimeMode::Native;
}

std::string WebRtcService::native_unavailable_json() const {
    const auto status = native_status();
    std::ostringstream body;
    body << "{";
    body << "\"ok\":false,";
    body << "\"error\":\"native WebRTC media engine unavailable\",";
    body << "\"mode\":\"" << mode_to_string(status.configured_mode) << "\",";
    body << "\"selected_runtime\":\"" << status.selected_runtime << "\",";
    body << "\"compiled_with_dependency\":" << (status.compiled_with_dependency ? "true" : "false") << ",";
    body << "\"dependency_ready\":" << (status.dependency_ready ? "true" : "false") << ",";
    body << "\"peer_factory_ready\":" << (status.peer_factory_ready ? "true" : "false") << ",";
    body << "\"media_engine_ready\":" << (status.media_engine_ready ? "true" : "false") << ",";
    body << "\"dependency_root\":\"" << status.dependency_root << "\",";
    body << "\"detail\":\"" << status.detail << "\"";
    body << "}";
    return body.str();
}

NativeOfferResult WebRtcService::create_native_play_offer(const std::string& stream_key) {
    NativeOfferResult result;
#if !OTTS_WEBRTC_DATACHANNEL
    (void)stream_key;
    result.error = "libdatachannel backend is not compiled";
    return result;
#else
    otts::rtmp::StreamRegistry* registry = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
    }
    if (registry == nullptr) {
        result.error = "stream registry is not attached";
        return result;
    }
    if (stream_key.empty()) {
        result.error = "missing stream_key";
        return result;
    }

    auto session = std::make_shared<NativeSession>();
    session->session_id = make_session_id();
    session->stream_key = stream_key;
    session->direction = SessionDirection::Play;
    session->registry = registry;
    session->started_at_epoch_ms = now_epoch_ms();

    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    session->pc = pc;

    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool gathered = false;
    std::string local_sdp;

    pc->onStateChange([this, weak = std::weak_ptr<NativeSession>(session)](rtc::PeerConnection::State state) {
        if (auto locked = weak.lock()) {
            locked->open.store(state == rtc::PeerConnection::State::Connected);
            std::string error;
            if (state == rtc::PeerConnection::State::Failed) {
                error = "peer connection failed";
            }
            update_session_state(
                locked->session_id,
                rtc_state_to_session_state(state),
                rtc_state_to_transport_string(state),
                error);
        }
    });
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) {
            return;
        }
        std::lock_guard<std::mutex> lock(wait_mutex);
        if (const auto description = pc->localDescription()) {
            local_sdp = std::string(description.value());
        }
        gathered = true;
        wait_cv.notify_one();
    });

    auto attach_play_callback = std::make_shared<std::function<void()>>();
    *attach_play_callback = [weak = std::weak_ptr<NativeSession>(session), registry]() {
        auto locked = weak.lock();
        if (!locked || !locked->video_track_open.load() || !locked->audio_track_open.load()) {
            return;
        }
        bool expected = false;
        if (!locked->play_callback_registered.compare_exchange_strong(expected, true)) {
            return;
        }
        const auto callback_id = static_cast<otts::rtmp::StreamRegistry::CallbackId>(now_epoch_ms());
        locked->callback_id = callback_id;
        registry->update_external_viewers(locked->stream_key, otts::media::StreamSource::Whip, "cpp-webrtc-native", 1);
        locked->start_play_sender("native-offer WHEP");
        registry->add_callback_subscriber(locked->stream_key, callback_id, [weak](const otts::rtmp::MediaMessage& message) {
            if (auto locked = weak.lock()) {
                locked->enqueue_play_media(message);
            }
        });
        otts::core::log_info("webrtc_native", "native-offer WHEP tracks open; subscribed to stream key=" + locked->stream_key);
    };

    constexpr std::uint8_t video_payload_type = 102;
    constexpr std::uint8_t audio_payload_type = 111;
    constexpr rtc::SSRC video_ssrc = 0x51545331;
    constexpr rtc::SSRC audio_ssrc = 0x51545332;
    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(video_payload_type);
    video.addSSRC(video_ssrc, "otts-video", "otts-stream", "otts-video");
    auto video_track = pc->addTrack(video);
    auto video_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        video_ssrc,
        "otts-video",
        video_payload_type,
        rtc::H264RtpPacketizer::ClockRate);
    auto video_packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, video_rtp_config);
    auto video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(video_rtp_config);
    video_packetizer->addToChain(video_sr_reporter);
    video_packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    video_track->setMediaHandler(video_packetizer);
    video_track->onOpen([weak = std::weak_ptr<NativeSession>(session), attach_play_callback]() {
        if (auto locked = weak.lock()) {
            locked->video_track_open.store(true);
            (*attach_play_callback)();
        }
    });
    video_track->onClosed([weak = std::weak_ptr<NativeSession>(session)]() {
        if (auto locked = weak.lock()) {
            locked->detach_play_subscription();
        }
    });
    session->video_track = video_track;
    session->video_sr_reporter = video_sr_reporter;

    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(audio_payload_type);
    audio.addSSRC(audio_ssrc, "otts-audio", "otts-stream", "otts-audio");
    auto audio_track = pc->addTrack(audio);
    auto audio_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        audio_ssrc,
        "otts-audio",
        audio_payload_type,
        rtc::OpusRtpPacketizer::DefaultClockRate);
    auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp_config);
    auto audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(audio_rtp_config);
    audio_packetizer->addToChain(audio_sr_reporter);
    audio_track->setMediaHandler(audio_packetizer);
    audio_track->onOpen([weak = std::weak_ptr<NativeSession>(session), attach_play_callback]() {
        if (auto locked = weak.lock()) {
            locked->audio_track_open.store(true);
            (*attach_play_callback)();
        }
    });
    audio_track->onClosed([weak = std::weak_ptr<NativeSession>(session)]() {
        if (auto locked = weak.lock()) {
            locked->detach_play_subscription();
        }
    });
    session->audio_track = audio_track;
    session->audio_sr_reporter = audio_sr_reporter;

    upsert_native_protocol_session(
        registry,
        session->session_id,
        stream_key,
        SessionDirection::Play,
        "offering",
        "native-cxx-libdatachannel-offer",
        "h264/h265+aac(core)/opus(webrtc)",
        session->started_at_epoch_ms);

    SessionStateData state;
    state.session_id = session->session_id;
    state.stream_key = stream_key;
    state.direction = SessionDirection::Play;
    state.state = SessionState::AwaitingTransport;
    state.created_at_epoch_ms = session->started_at_epoch_ms;
    state.updated_at_epoch_ms = state.created_at_epoch_ms;
    state.transport_state = "offering";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[state.session_id] = state;
        native_sessions_[state.session_id] = session;
    }

    try {
        pc->setLocalDescription();
        {
            std::unique_lock<std::mutex> lock(wait_mutex);
            wait_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return gathered; });
        }
        if (local_sdp.empty()) {
            if (const auto description = pc->localDescription()) {
                local_sdp = std::string(description.value());
            }
        }
        if (local_sdp.empty()) {
            result.error = "failed to create local SDP offer";
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& stored = sessions_[session->session_id];
            stored.offer_sdp = local_sdp;
            stored.updated_at_epoch_ms = now_epoch_ms();
        }
        result.ok = true;
        result.session_id = session->session_id;
        result.answer_sdp = local_sdp;
        return result;
    } catch (const std::exception& exc) {
        result.error = exc.what();
        return result;
    }
#endif
}

bool WebRtcService::set_native_answer(const std::string& session_id, const std::string& answer_sdp) {
#if !OTTS_WEBRTC_DATACHANNEL
    (void)session_id;
    (void)answer_sdp;
    return false;
#else
    std::shared_ptr<NativeSession> native_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = native_sessions_.find(session_id);
        if (it == native_sessions_.end()) {
            return false;
        }
        native_session = it->second;
        auto& state = sessions_[session_id];
        state.answer_sdp = answer_sdp;
        state.updated_at_epoch_ms = now_epoch_ms();
    }
    try {
        native_session->pc->setRemoteDescription(rtc::Description(answer_sdp, "answer"));
        return true;
    } catch (const std::exception& exc) {
        fail_session(session_id, exc.what());
        return false;
    }
#endif
}

NativeOfferResult WebRtcService::handle_native_offer_locked(
    SessionDirection direction,
    const std::string& stream_key,
    const std::string& offer_sdp) {
    NativeOfferResult result;
#if !OTTS_WEBRTC_DATACHANNEL
    (void)direction;
    (void)stream_key;
    (void)offer_sdp;
    result.error = "libdatachannel backend is not compiled";
    return result;
#else
    otts::rtmp::StreamRegistry* registry = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry = registry_;
    }
    if (registry == nullptr) {
        result.error = "stream registry is not attached";
        return result;
    }
    if (stream_key.empty() || offer_sdp.empty()) {
        result.error = "missing stream_key or SDP offer";
        return result;
    }

    auto session = std::make_shared<NativeSession>();
    session->session_id = make_session_id();
    session->stream_key = stream_key;
    session->direction = direction;
    session->registry = registry;
    session->started_at_epoch_ms = now_epoch_ms();

    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    session->pc = pc;

    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool gathered = false;
    std::string local_sdp;

    pc->onStateChange([this, weak = std::weak_ptr<NativeSession>(session)](rtc::PeerConnection::State state) {
        if (auto locked = weak.lock()) {
            const auto connected = state == rtc::PeerConnection::State::Connected;
            locked->open.store(connected);
            std::string error;
            if (state == rtc::PeerConnection::State::Failed) {
                error = "peer connection failed";
            }
            update_session_state(
                locked->session_id,
                rtc_state_to_session_state(state),
                rtc_state_to_transport_string(state),
                error);
        }
    });
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) {
            return;
        }
        std::lock_guard<std::mutex> lock(wait_mutex);
        if (const auto description = pc->localDescription()) {
            local_sdp = std::string(description.value());
        }
        gathered = true;
        wait_cv.notify_one();
    });

    try {
        bool remote_description_set = false;
        const auto negotiated_payload_types = parse_offer_payload_types(offer_sdp);
        if (direction == SessionDirection::Play) {
            auto source_codec = otts::media::CodecId::Avc;
            for (const auto& stream : registry->snapshots()) {
                if (stream.stream_key == stream_key && stream.video_codec == "h265") {
                    source_codec = otts::media::CodecId::Hevc;
                    break;
                }
            }
            auto wire_codec = otts::media::CodecId::Avc;
            if (source_codec == otts::media::CodecId::Hevc) {
                if (negotiated_payload_types.preferred_video_codec == otts::media::CodecId::Hevc &&
                    negotiated_payload_types.has_h265) {
                    wire_codec = otts::media::CodecId::Hevc;
                } else if (!negotiated_payload_types.has_h264 && negotiated_payload_types.has_h265) {
                    wire_codec = otts::media::CodecId::Hevc;
                }
            }
            session->source_video_codec = source_codec;
            session->video_codec = wire_codec;
            const auto video_payload_type = wire_codec == otts::media::CodecId::Hevc
                ? negotiated_payload_types.h265 : negotiated_payload_types.h264;
            const auto audio_payload_type = negotiated_payload_types.opus;
            constexpr rtc::SSRC video_ssrc = 0x51545331;
            constexpr rtc::SSRC audio_ssrc = 0x51545332;
            otts::core::log_info(
                "webrtc_native",
                "WHEP negotiated payload types key=" + stream_key +
                    " source=" + otts::media::to_string(source_codec) +
                    " wire=" + otts::media::to_string(wire_codec) + ":" + std::to_string(video_payload_type) +
                    " opus=" + std::to_string(audio_payload_type) +
                    " video_mid=" + negotiated_payload_types.video_mid +
                    " audio_mid=" + negotiated_payload_types.audio_mid);
            auto attach_play_callback = std::make_shared<std::function<void()>>();
            *attach_play_callback = [weak = std::weak_ptr<NativeSession>(session), registry]() {
                auto locked = weak.lock();
                if (!locked || !locked->open.load() || !locked->video_track || !locked->audio_track) {
                    return;
                }
                bool expected = false;
                if (!locked->play_callback_registered.compare_exchange_strong(expected, true)) {
                    return;
                }
                const auto callback_id = static_cast<otts::rtmp::StreamRegistry::CallbackId>(now_epoch_ms());
                locked->callback_id = callback_id;
                registry->update_external_viewers(locked->stream_key, otts::media::StreamSource::Whip, "cpp-webrtc-native", 1);
                locked->start_play_sender("WHEP");
                registry->add_callback_subscriber(locked->stream_key, callback_id, [weak](const otts::rtmp::MediaMessage& message) {
                    if (auto locked = weak.lock()) {
                        locked->enqueue_play_media(message);
                    }
                });
                otts::core::log_info("webrtc_native", "WHEP connected; subscribed to stream key=" + locked->stream_key);
            };
            pc->onStateChange([this, weak = std::weak_ptr<NativeSession>(session), attach_play_callback](rtc::PeerConnection::State state) {
                if (auto locked = weak.lock()) {
                    const auto connected = state == rtc::PeerConnection::State::Connected;
                    locked->open.store(connected);
                    std::string error;
                    if (state == rtc::PeerConnection::State::Failed) {
                        error = "peer connection failed";
                    }
                    update_session_state(
                        locked->session_id,
                        rtc_state_to_session_state(state),
                        rtc_state_to_transport_string(state),
                        error);
                    if (connected) {
                        (*attach_play_callback)();
                    }
                }
            });
            rtc::Description::Video video(negotiated_payload_types.video_mid, rtc::Description::Direction::SendOnly);
            if (wire_codec == otts::media::CodecId::Hevc) video.addH265Codec(video_payload_type);
            else video.addH264Codec(video_payload_type);
            video.addSSRC(video_ssrc, "otts-video", "otts-stream", "otts-video");
            auto video_track = pc->addTrack(video);
            auto video_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
                video_ssrc,
                "otts-video",
                video_payload_type,
                90000);
            auto video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(video_rtp_config);
            if (wire_codec == otts::media::CodecId::Hevc) {
                auto packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, video_rtp_config);
                packetizer->addToChain(video_sr_reporter);
                packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
                video_track->setMediaHandler(packetizer);
            } else {
                auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, video_rtp_config);
                packetizer->addToChain(video_sr_reporter);
                packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
                video_track->setMediaHandler(packetizer);
            }
            session->video_track = video_track;
            session->video_sr_reporter = video_sr_reporter;
            video_track->onOpen([weak = std::weak_ptr<NativeSession>(session), attach_play_callback]() {
                if (auto locked = weak.lock()) {
                    locked->video_track_open.store(true);
                    (*attach_play_callback)();
                }
            });
            video_track->onClosed([weak = std::weak_ptr<NativeSession>(session)]() {
                if (auto locked = weak.lock()) {
                    locked->video_track_open.store(false);
                }
            });

            rtc::Description::Audio audio(negotiated_payload_types.audio_mid, rtc::Description::Direction::SendOnly);
            audio.addOpusCodec(audio_payload_type);
            audio.addSSRC(audio_ssrc, "otts-audio", "otts-stream", "otts-audio");
            auto audio_track = pc->addTrack(audio);
            auto audio_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
                audio_ssrc,
                "otts-audio",
                audio_payload_type,
                rtc::OpusRtpPacketizer::DefaultClockRate);
            auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp_config);
            auto audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(audio_rtp_config);
            audio_packetizer->addToChain(audio_sr_reporter);
            audio_track->setMediaHandler(audio_packetizer);
            session->audio_track = audio_track;
            session->audio_sr_reporter = audio_sr_reporter;
            audio_track->onOpen([weak = std::weak_ptr<NativeSession>(session), attach_play_callback]() {
                if (auto locked = weak.lock()) {
                    locked->audio_track_open.store(true);
                    (*attach_play_callback)();
                }
            });
            audio_track->onClosed([weak = std::weak_ptr<NativeSession>(session)]() {
                if (auto locked = weak.lock()) {
                    locked->audio_track_open.store(false);
                }
            });
        } else {
            session->video_codec = negotiated_payload_types.preferred_video_codec == otts::media::CodecId::Hevc
                ? otts::media::CodecId::Hevc : otts::media::CodecId::Avc;
            session->source_video_codec = session->video_codec;
            registry->begin_external_publish(
                stream_key, otts::media::StreamSource::Whip, "cpp-webrtc-native");
            registry->upsert_external_stream(
                stream_key, otts::media::StreamSource::Whip, "aac", otts::media::to_string(session->video_codec),
                "cpp-webrtc-native", true);
            pc->onTrack([session, registry](std::shared_ptr<rtc::Track> track) {
                const auto media_type = track->description().type();
                if (media_type == "audio") {
                    session->audio_track = track;
                    track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
                    track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                    auto base_ts = std::make_shared<std::optional<std::uint32_t>>();
                    track->onFrame([session, registry, base_ts](rtc::binary frame, rtc::FrameInfo info) {
                        auto opus = bytes_from_rtc_binary(frame);
                        if (opus.empty()) {
                            return;
                        }
                        const auto timestamp = media_timestamp_ms_from_rtp(
                            info,
                            base_ts,
                            rtc::OpusRtpPacketizer::DefaultClockRate);
                        session->audio_frames.fetch_add(1);
                        session->audio_bytes.fetch_add(opus.size());
                        session->publish_opus_as_aac(opus, timestamp);
                    });
                    return;
                }

                session->video_track = track;
                if (session->video_codec == otts::media::CodecId::Hevc) {
                    track->setMediaHandler(std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
                } else {
                    track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
                }
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                auto known_sets = std::make_shared<otts::codec::ParameterSets>();
                auto base_ts = std::make_shared<std::optional<std::uint32_t>>();
                track->onFrame([session, registry, known_sets, base_ts](rtc::binary frame, rtc::FrameInfo info) {
                    auto annexb = bytes_from_rtc_binary(frame);
                    if (annexb.empty()) {
                        return;
                    }
                    const auto sets = otts::codec::extract_parameter_sets(annexb, session->video_codec);
                    if (!sets.vps.empty()) {
                        known_sets->vps = sets.vps;
                    }
                    if (!sets.sps.empty()) {
                        known_sets->sps = sets.sps;
                    }
                    if (!sets.pps.empty()) {
                        known_sets->pps = sets.pps;
                    }
                    const auto timestamp = media_timestamp_ms_from_rtp(
                        info,
                        base_ts,
                        90000);
                    if (known_sets->complete(session->video_codec)) {
                        otts::rtmp::MediaMessage config;
                        config.timestamp = timestamp;
                        config.type_id = 9;
                        config.payload = otts::codec::make_flv_video_config(session->video_codec, *known_sets);
                        registry->publish_external_media(
                            session->stream_key,
                            otts::media::StreamSource::Whip,
                            "cpp-webrtc-native",
                            config);
                    }
                    otts::rtmp::MediaMessage media;
                    media.timestamp = timestamp;
                    media.type_id = 9;
                    media.payload = otts::codec::make_flv_video_sample(
                        session->video_codec, annexb, otts::codec::is_keyframe(annexb, session->video_codec));
                    if (media.payload.size() > 5) {
                        session->video_frames.fetch_add(1);
                        session->video_bytes.fetch_add(annexb.size());
                        registry->publish_external_media(
                            session->stream_key,
                            otts::media::StreamSource::Whip,
                            "cpp-webrtc-native",
                            media);
                    }
                });
            });
        }

        upsert_native_protocol_session(
            registry,
            session->session_id,
            stream_key,
            direction,
            "answering",
            "native-cxx-libdatachannel",
            "h264/h265+aac(core)/opus(webrtc)",
            session->started_at_epoch_ms);

        SessionStateData state;
        state.session_id = session->session_id;
        state.stream_key = stream_key;
        state.direction = direction;
        state.state = SessionState::AwaitingTransport;
        state.offer_sdp = offer_sdp;
        state.created_at_epoch_ms = session->started_at_epoch_ms;
        state.updated_at_epoch_ms = state.created_at_epoch_ms;
        state.transport_state = "answering";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[state.session_id] = state;
            native_sessions_[state.session_id] = session;
        }

        if (!remote_description_set) {
            pc->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
        }
        pc->setLocalDescription();

        {
            std::unique_lock<std::mutex> lock(wait_mutex);
            wait_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return gathered; });
        }
        if (local_sdp.empty()) {
            if (const auto description = pc->localDescription()) {
                local_sdp = std::string(description.value());
            }
        }
        if (local_sdp.empty()) {
            result.error = "failed to create local SDP answer";
            return result;
        }

        upsert_native_protocol_session(
            registry,
            session->session_id,
            stream_key,
            direction,
            "running",
            "native-cxx-libdatachannel",
            "h264/h265+aac(core)/opus(webrtc)",
            session->started_at_epoch_ms);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& stored = sessions_[session->session_id];
            stored.answer_sdp = local_sdp;
            stored.updated_at_epoch_ms = now_epoch_ms();
        }

        result.ok = true;
        result.session_id = session->session_id;
        result.answer_sdp = local_sdp;
        return result;
    } catch (const std::exception& exc) {
        result.error = exc.what();
        return result;
    }
#endif
}

std::string WebRtcService::make_session_id() {
    std::mt19937_64 rng(std::random_device{}());
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
}

std::uint64_t WebRtcService::now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string WebRtcService::mode_to_string(RuntimeMode mode) {
    switch (mode) {
        case RuntimeMode::Gateway:
            return "gateway";
        case RuntimeMode::Auto:
            return "auto";
        case RuntimeMode::Native:
            return "native";
    }
    return "gateway";
}

SessionSnapshot WebRtcService::make_snapshot(
    const SessionStateData& state,
    std::uint64_t video_frames,
    std::uint64_t video_bytes,
    std::uint64_t audio_frames,
    std::uint64_t audio_bytes) {
    SessionSnapshot snapshot;
    snapshot.session_id = state.session_id;
    snapshot.stream_key = state.stream_key;
    snapshot.direction = direction_to_string(state.direction);
    snapshot.state = state_to_string(state.state);
    snapshot.offer_size = state.offer_sdp.size();
    snapshot.answer_size = state.answer_sdp.size();
    snapshot.created_at_epoch_ms = state.created_at_epoch_ms;
    snapshot.updated_at_epoch_ms = state.updated_at_epoch_ms;
    snapshot.video_frames = video_frames;
    snapshot.video_bytes = video_bytes;
    snapshot.audio_frames = audio_frames;
    snapshot.audio_bytes = audio_bytes;
    snapshot.transport_state = state.transport_state;
    snapshot.last_error = state.last_error;
    return snapshot;
}

}  // namespace otts::webrtc
