#include "otts/webrtc/webrtc_service.hpp"

#include "otts/core/logger.hpp"
#include "otts/rtmp/stream_registry.hpp"

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

void write_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

struct NalSets {
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

struct WebRtcPayloadTypes {
    std::uint8_t h264{102};
    std::uint8_t opus{111};
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
                found_h264 = true;
            }
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

std::optional<std::pair<std::size_t, std::size_t>> start_code_at(
    const std::vector<std::uint8_t>& data,
    std::size_t pos) {
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        return std::make_pair(pos, std::size_t{3});
    }
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) {
        return std::make_pair(pos, std::size_t{4});
    }
    return std::nullopt;
}

NalSets extract_parameter_sets_from_annexb(const std::vector<std::uint8_t>& annexb) {
    NalSets sets;
    std::size_t i = 0;
    while (i < annexb.size()) {
        const auto code = start_code_at(annexb, i);
        if (!code) {
            ++i;
            continue;
        }
        const auto nal_start = code->first + code->second;
        auto nal_end = nal_start;
        while (nal_end < annexb.size() && !start_code_at(annexb, nal_end)) {
            ++nal_end;
        }
        if (nal_end > nal_start) {
            const auto nal_type = annexb[nal_start] & 0x1f;
            if (nal_type == 7) {
                sets.sps.assign(annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
            } else if (nal_type == 8) {
                sets.pps.assign(annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
            }
        }
        i = nal_end;
    }
    return sets;
}

bool annexb_has_idr(const std::vector<std::uint8_t>& annexb) {
    std::size_t i = 0;
    while (i < annexb.size()) {
        const auto code = start_code_at(annexb, i);
        if (!code) {
            ++i;
            continue;
        }
        const auto nal = code->first + code->second;
        if (nal < annexb.size() && (annexb[nal] & 0x1f) == 5) {
            return true;
        }
        i = nal + 1;
    }
    return false;
}

std::vector<std::uint8_t> annexb_to_avcc(const std::vector<std::uint8_t>& annexb) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < annexb.size()) {
        const auto code = start_code_at(annexb, i);
        if (!code) {
            ++i;
            continue;
        }
        const auto nal_start = code->first + code->second;
        auto nal_end = nal_start;
        while (nal_end < annexb.size() && !start_code_at(annexb, nal_end)) {
            ++nal_end;
        }
        const auto nal_size = nal_end - nal_start;
        if (nal_size > 0) {
            write_be32(out, static_cast<std::uint32_t>(nal_size));
            out.insert(out.end(), annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
        }
        i = nal_end;
    }
    return out;
}

std::vector<std::uint8_t> avcc_to_annexb(const std::vector<std::uint8_t>& avcc) {
    std::vector<std::uint8_t> out;
    std::size_t offset = 0;
    while (offset + 4 <= avcc.size()) {
        const auto nal_size =
            (static_cast<std::uint32_t>(avcc[offset]) << 24) |
            (static_cast<std::uint32_t>(avcc[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(avcc[offset + 2]) << 8) |
            static_cast<std::uint32_t>(avcc[offset + 3]);
        offset += 4;
        if (nal_size == 0 || offset + nal_size > avcc.size()) {
            break;
        }
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(
            out.end(),
            avcc.begin() + static_cast<std::ptrdiff_t>(offset),
            avcc.begin() + static_cast<std::ptrdiff_t>(offset + nal_size));
        offset += nal_size;
    }
    return out;
}

otts::rtmp::MediaMessage make_avc_sequence_header(std::uint32_t timestamp, const NalSets& sets) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload = {0x17, 0x00, 0x00, 0x00, 0x00};
    message.payload.push_back(0x01);
    message.payload.push_back(sets.sps.size() > 1 ? sets.sps[1] : 0x64);
    message.payload.push_back(sets.sps.size() > 2 ? sets.sps[2] : 0x00);
    message.payload.push_back(sets.sps.size() > 3 ? sets.sps[3] : 0x1f);
    message.payload.push_back(0xff);
    message.payload.push_back(0xe1);
    write_be16(message.payload, static_cast<std::uint16_t>(sets.sps.size()));
    message.payload.insert(message.payload.end(), sets.sps.begin(), sets.sps.end());
    message.payload.push_back(0x01);
    write_be16(message.payload, static_cast<std::uint16_t>(sets.pps.size()));
    message.payload.insert(message.payload.end(), sets.pps.begin(), sets.pps.end());
    return message;
}

otts::rtmp::MediaMessage make_avc_nalu_message(std::uint32_t timestamp, const std::vector<std::uint8_t>& annexb) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload.push_back(static_cast<std::uint8_t>((annexb_has_idr(annexb) ? 0x10 : 0x20) | 0x07));
    message.payload.insert(message.payload.end(), {0x01, 0x00, 0x00, 0x00});
    auto avcc = annexb_to_avcc(annexb);
    message.payload.insert(message.payload.end(), avcc.begin(), avcc.end());
    return message;
}

otts::rtmp::MediaMessage make_opus_audio_message(std::uint32_t timestamp, const std::vector<std::uint8_t>& opus) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 8;
    message.message_stream_id = 1;
    message.payload.reserve(opus.size() + 1);
    message.payload.push_back(0xd0);
    message.payload.insert(message.payload.end(), opus.begin(), opus.end());
    return message;
}

std::vector<std::uint8_t> opus_audio_payload(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 2 || ((payload[0] >> 4) & 0x0f) != 13) {
        return {};
    }
    return std::vector<std::uint8_t>(payload.begin() + 1, payload.end());
}

std::vector<std::uint8_t> avc_sequence_to_sample(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    if (payload.size() < 13 || payload[1] != 0) {
        return out;
    }
    std::size_t offset = 5;
    if (offset + 6 > payload.size()) {
        return out;
    }
    const auto sps_count = static_cast<std::uint8_t>(payload[offset + 5] & 0x1f);
    offset += 6;
    for (std::uint8_t i = 0; i < sps_count && offset + 2 <= payload.size(); ++i) {
        const auto size = static_cast<std::size_t>((payload[offset] << 8) | payload[offset + 1]);
        offset += 2;
        if (offset + size > payload.size()) {
            return out;
        }
        write_be32(out, static_cast<std::uint32_t>(size));
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    if (offset >= payload.size()) {
        return out;
    }
    const auto pps_count = payload[offset++];
    for (std::uint8_t i = 0; i < pps_count && offset + 2 <= payload.size(); ++i) {
        const auto size = static_cast<std::size_t>((payload[offset] << 8) | payload[offset + 1]);
        offset += 2;
        if (offset + size > payload.size()) {
            return out;
        }
        write_be32(out, static_cast<std::uint32_t>(size));
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    return out;
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
        direction == SessionDirection::Publish ? "native-whip-h264-ingress" : "native-whep-h264-egress",
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
    std::vector<std::uint8_t> video_config_sample;
    std::uint64_t started_at_epoch_ms{0};
#if OTTS_WEBRTC_DATACHANNEL
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter;
    std::shared_ptr<rtc::RtcpSrReporter> audio_sr_reporter;
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
        native_status_.detail = "native WHIP/WHEP H.264 + Opus engine ready via libdatachannel";
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
            ? "native WebRTC H.264 + Opus media engine ready"
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
            "h264+opus",
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
        registry->add_callback_subscriber(locked->stream_key, callback_id, [weak](const otts::rtmp::MediaMessage& message) {
            auto locked = weak.lock();
            if (!locked) {
                return;
            }
            if (message.type_id == 8) {
                if (!locked->audio_track || !locked->audio_track_open.load()) {
                    return;
                }
                auto opus = opus_audio_payload(message.payload);
                if (opus.empty()) {
                    return;
                }
                rtc::FrameInfo info(std::chrono::duration<double, std::milli>(message.timestamp));
                try {
                    locked->audio_track->sendFrame(rtc_binary_from_bytes(opus), info);
                    locked->audio_frames.fetch_add(1);
                    locked->audio_bytes.fetch_add(opus.size());
                } catch (const std::exception& exc) {
                    const std::string error = exc.what();
                    otts::core::log_warn("webrtc_native", "native-offer WHEP audio sendFrame failed: " + error);
                    if (error.find("closed") != std::string::npos) {
                        locked->detach_play_subscription();
                    }
                }
                return;
            }
            if (!locked->video_track || !locked->video_track_open.load()) {
                return;
            }
            if (message.type_id != 9 || message.payload.size() < 5 || (message.payload[0] & 0x0f) != 7) {
                return;
            }
            std::vector<std::uint8_t> sample;
            bool is_video_keyframe = false;
            if (message.payload[1] == 0) {
                sample = avc_sequence_to_sample(message.payload);
                if (!sample.empty()) {
                    std::lock_guard<std::mutex> lock(locked->media_mutex);
                    locked->video_config_sample = sample;
                }
                is_video_keyframe = true;
            } else if (message.payload[1] == 1) {
                sample.assign(message.payload.begin() + 5, message.payload.end());
                is_video_keyframe = ((message.payload[0] >> 4) & 0x0f) == 1 || annexb_has_idr(avcc_to_annexb(sample));
            } else {
                return;
            }
            if (!is_video_keyframe && !locked->video_keyframe_seen.load()) {
                return;
            }
            if (is_video_keyframe) {
                std::vector<std::uint8_t> with_config;
                {
                    std::lock_guard<std::mutex> lock(locked->media_mutex);
                    with_config = locked->video_config_sample;
                }
                if (!with_config.empty()) {
                    with_config.insert(with_config.end(), sample.begin(), sample.end());
                    sample = std::move(with_config);
                }
                locked->video_keyframe_seen.store(true);
            }
            sample = avcc_to_annexb(sample);
            if (sample.empty()) {
                return;
            }
            rtc::FrameInfo info(std::chrono::duration<double, std::milli>(message.timestamp));
            info.isKeyFrame = is_video_keyframe;
            try {
                locked->video_track->sendFrame(rtc_binary_from_bytes(sample), info);
                locked->video_frames.fetch_add(1);
                locked->video_bytes.fetch_add(sample.size());
            } catch (const std::exception& exc) {
                const std::string error = exc.what();
                otts::core::log_warn("webrtc_native", "native-offer WHEP sendFrame failed: " + error);
                if (error.find("closed") != std::string::npos) {
                    locked->detach_play_subscription();
                }
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
        "h264+opus",
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
        if (direction == SessionDirection::Play) {
            const auto negotiated_payload_types = parse_offer_payload_types(offer_sdp);
            const auto video_payload_type = negotiated_payload_types.h264;
            const auto audio_payload_type = negotiated_payload_types.opus;
            constexpr rtc::SSRC video_ssrc = 0x51545331;
            constexpr rtc::SSRC audio_ssrc = 0x51545332;
            otts::core::log_info(
                "webrtc_native",
                "WHEP negotiated payload types key=" + stream_key +
                    " h264=" + std::to_string(video_payload_type) +
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
                registry->add_callback_subscriber(locked->stream_key, callback_id, [weak](const otts::rtmp::MediaMessage& message) {
                    auto locked = weak.lock();
                    if (!locked) {
                        return;
                    }
                    if (message.type_id == 8) {
                        if (!locked->audio_track) {
                            return;
                        }
                        auto opus = opus_audio_payload(message.payload);
                        if (opus.empty()) {
                            return;
                        }
                        rtc::FrameInfo info(std::chrono::duration<double, std::milli>(message.timestamp));
                        try {
                            locked->audio_track->sendFrame(rtc_binary_from_bytes(opus), info);
                            locked->audio_frames.fetch_add(1);
                            locked->audio_bytes.fetch_add(opus.size());
                        } catch (const std::exception& exc) {
                            const std::string error = exc.what();
                            otts::core::log_warn("webrtc_native", "WHEP audio sendFrame failed: " + error);
                            if (error.find("closed") != std::string::npos) {
                                locked->detach_play_subscription();
                            }
                        }
                        return;
                    }
                    if (!locked->video_track) {
                        return;
                    }
                    if (message.type_id != 9 || message.payload.size() < 5 || (message.payload[0] & 0x0f) != 7) {
                        return;
                    }
                    std::vector<std::uint8_t> sample;
                    bool is_video_keyframe = false;
                    if (message.payload[1] == 0) {
                        sample = avc_sequence_to_sample(message.payload);
                        if (!sample.empty()) {
                            std::lock_guard<std::mutex> lock(locked->media_mutex);
                            locked->video_config_sample = sample;
                        }
                        is_video_keyframe = true;
                    } else if (message.payload[1] == 1) {
                        sample.assign(message.payload.begin() + 5, message.payload.end());
                        is_video_keyframe = ((message.payload[0] >> 4) & 0x0f) == 1 || annexb_has_idr(avcc_to_annexb(sample));
                    } else {
                        return;
                    }
                    if (!is_video_keyframe && !locked->video_keyframe_seen.load()) {
                        return;
                    }
                    if (is_video_keyframe) {
                        std::vector<std::uint8_t> with_config;
                        {
                            std::lock_guard<std::mutex> lock(locked->media_mutex);
                            with_config = locked->video_config_sample;
                        }
                        if (!with_config.empty()) {
                            with_config.insert(with_config.end(), sample.begin(), sample.end());
                            sample = std::move(with_config);
                        }
                        locked->video_keyframe_seen.store(true);
                    }
                    sample = avcc_to_annexb(sample);
                    if (sample.empty()) {
                        return;
                    }
                    rtc::FrameInfo info(std::chrono::duration<double, std::milli>(message.timestamp));
                    info.isKeyFrame = is_video_keyframe;
                    try {
                        locked->video_track->sendFrame(rtc_binary_from_bytes(sample), info);
                        locked->video_frames.fetch_add(1);
                        locked->video_bytes.fetch_add(sample.size());
                    } catch (const std::exception& exc) {
                        const std::string error = exc.what();
                        otts::core::log_warn("webrtc_native", "WHEP sendFrame failed: " + error);
                        if (error.find("closed") != std::string::npos) {
                            locked->detach_play_subscription();
                        }
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
            registry->upsert_external_stream(stream_key, otts::media::StreamSource::Whip, "opus", "h264", "cpp-webrtc-native", true);
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
                        registry->publish_external_media(
                            session->stream_key,
                            otts::media::StreamSource::Whip,
                            "cpp-webrtc-native",
                            make_opus_audio_message(timestamp, opus));
                    });
                    return;
                }

                session->video_track = track;
                track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                auto known_sets = std::make_shared<NalSets>();
                auto base_ts = std::make_shared<std::optional<std::uint32_t>>();
                track->onFrame([session, registry, known_sets, base_ts](rtc::binary frame, rtc::FrameInfo info) {
                    auto annexb = bytes_from_rtc_binary(frame);
                    if (annexb.empty()) {
                        return;
                    }
                    const auto sets = extract_parameter_sets_from_annexb(annexb);
                    if (!sets.sps.empty()) {
                        known_sets->sps = sets.sps;
                    }
                    if (!sets.pps.empty()) {
                        known_sets->pps = sets.pps;
                    }
                    const auto timestamp = media_timestamp_ms_from_rtp(
                        info,
                        base_ts,
                        rtc::H264RtpPacketizer::ClockRate);
                    if (!known_sets->sps.empty() && !known_sets->pps.empty()) {
                        registry->publish_external_media(
                            session->stream_key,
                            otts::media::StreamSource::Whip,
                            "cpp-webrtc-native",
                            make_avc_sequence_header(timestamp, *known_sets));
                    }
                    auto media = make_avc_nalu_message(timestamp, annexb);
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
            "h264+opus",
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
            "h264+opus",
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
