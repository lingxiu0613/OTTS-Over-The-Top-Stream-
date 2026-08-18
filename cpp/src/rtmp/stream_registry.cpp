#include "otts/rtmp/stream_registry.hpp"

#include "otts/core/logger.hpp"
#include "otts/rtmp/rtmp_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace otts::rtmp {

namespace {

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static constexpr std::array<char, 64> kTable{
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
    };

    std::string output;
    output.reserve(((data.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto b0 = data[i];
        const auto b1 = i + 1 < data.size() ? data[i + 1] : 0;
        const auto b2 = i + 2 < data.size() ? data[i + 2] : 0;
        const auto triple = static_cast<std::uint32_t>(b0) << 16 |
                            static_cast<std::uint32_t>(b1) << 8 |
                            static_cast<std::uint32_t>(b2);

        output.push_back(kTable[(triple >> 18) & 0x3F]);
        output.push_back(kTable[(triple >> 12) & 0x3F]);
        output.push_back(i + 1 < data.size() ? kTable[(triple >> 6) & 0x3F] : '=');
        output.push_back(i + 2 < data.size() ? kTable[triple & 0x3F] : '=');
    }

    return output;
}


std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::uint32_t aac_sample_rate_from_index(std::uint8_t index) {
    static constexpr std::array<std::uint32_t, 13> kRates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    return index < kRates.size() ? kRates[index] : 0;
}

}  // namespace

void StreamRegistry::register_publisher(const std::string& stream_key, const std::shared_ptr<RtmpSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& stream = streams_[stream_key];
    stream.publisher = session;
    stream.source = otts::media::StreamSource::Rtmp;
    stream.audio_track.kind = otts::media::MediaKind::Audio;
    stream.video_track.kind = otts::media::MediaKind::Video;
    otts::core::log_info("stream_registry", "registered publisher key=" + stream_key);
    persist_state_locked();
}

void StreamRegistry::unregister_publisher(const std::shared_ptr<RtmpSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        auto publisher = it->second.publisher.lock();
        if (publisher == session) {
            it->second.publisher.reset();
        }

        if (it->second.publisher.expired() && it->second.subscribers.empty()) {
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
    persist_state_locked();
}

void StreamRegistry::add_subscriber(const std::string& stream_key, const std::shared_ptr<RtmpSession>& session) {
    std::vector<MediaMessage> cached_messages;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stream = streams_[stream_key];
        stream.subscribers.push_back(session);

        if (stream.metadata) {
            cached_messages.push_back(*stream.metadata);
        }
        if (stream.audio_sequence_header) {
            cached_messages.push_back(*stream.audio_sequence_header);
        }
        if (stream.video_sequence_header) {
            cached_messages.push_back(*stream.video_sequence_header);
        }
        for (const auto& packet : stream.gop_cache.packets()) {
            MediaMessage media_message;
            media_message.timestamp = packet.timestamp_ms;
            media_message.type_id = packet.kind == otts::media::MediaKind::Video ? 9 : 8;
            media_message.message_stream_id = packet.message_stream_id;
            media_message.payload = packet.payload;
            cached_messages.push_back(std::move(media_message));
        }
        otts::core::log_info(
            "stream_registry",
            "add subscriber key=" + stream_key + " cached_messages=" + std::to_string(cached_messages.size()));
        persist_state_locked();
    }

    for (const auto& message : cached_messages) {
        session->send_media(message);
    }
}

void StreamRegistry::remove_subscriber(const std::shared_ptr<RtmpSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        auto& subscribers = it->second.subscribers;
        subscribers.erase(
            std::remove_if(
                subscribers.begin(),
                subscribers.end(),
                [&](const std::weak_ptr<RtmpSession>& weak) {
                    auto locked = weak.lock();
                    return !locked || locked == session;
                }),
            subscribers.end());

        if (it->second.publisher.expired() && subscribers.empty()) {
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
    persist_state_locked();
}

void StreamRegistry::add_callback_subscriber(
    const std::string& stream_key,
    CallbackId callback_id,
    MediaCallback callback) {
    std::vector<MediaMessage> cached_messages;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stream = streams_[stream_key];
        stream.callback_subscribers.push_back(CallbackSubscriber{callback_id, callback});

        if (stream.metadata) {
            cached_messages.push_back(*stream.metadata);
        }
        if (stream.audio_sequence_header) {
            cached_messages.push_back(*stream.audio_sequence_header);
        }
        if (stream.video_sequence_header) {
            cached_messages.push_back(*stream.video_sequence_header);
        }
        for (const auto& packet : stream.gop_cache.packets()) {
            MediaMessage media_message;
            media_message.timestamp = packet.timestamp_ms;
            media_message.type_id = packet.kind == otts::media::MediaKind::Video ? 9 : 8;
            media_message.message_stream_id = packet.message_stream_id;
            media_message.payload = packet.payload;
            cached_messages.push_back(std::move(media_message));
        }
        persist_state_locked();
    }

    for (const auto& message : cached_messages) {
        callback(message);
    }
}

void StreamRegistry::remove_callback_subscriber(const std::string& stream_key, CallbackId callback_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_key);
    if (it == streams_.end()) {
        return;
    }

    auto& callbacks = it->second.callback_subscribers;
    callbacks.erase(
        std::remove_if(
            callbacks.begin(),
            callbacks.end(),
            [&](const CallbackSubscriber& subscriber) { return subscriber.id == callback_id; }),
        callbacks.end());

    if (it->second.publisher.expired() && it->second.subscribers.empty() && callbacks.empty()) {
        streams_.erase(it);
    }

    persist_state_locked();
}

void StreamRegistry::publish_media(const std::string& stream_key, const MediaMessage& message) {
    std::vector<std::shared_ptr<RtmpSession>> subscribers;
    std::vector<MediaCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_key);
        if (it == streams_.end()) {
            return;
        }
        publish_media_locked(stream_key, it->second, message, subscribers, callbacks);
        persist_state_locked();
    }

    otts::core::log_debug(
        "stream_registry",
        "fanout key=" + stream_key + " type=" + std::to_string(message.type_id) +
            " subscribers=" + std::to_string(subscribers.size()));

    for (const auto& subscriber : subscribers) {
        subscriber->send_media(message);
    }

    for (const auto& callback : callbacks) {
        callback(message);
    }
}

void StreamRegistry::publish_external_media(
    const std::string& stream_key,
    otts::media::StreamSource source,
    const std::string& managed_by,
    const MediaMessage& message) {
    std::vector<std::shared_ptr<RtmpSession>> subscribers;
    std::vector<MediaCallback> callbacks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stream = streams_[stream_key];
        if (stream.publisher.expired()) {
            stream.source = source;
        }
        stream.ingest_origin = source;
        stream.managed_by = managed_by;
        stream.external_publisher_active = true;
        stream.audio_track.kind = otts::media::MediaKind::Audio;
        stream.video_track.kind = otts::media::MediaKind::Video;
        publish_media_locked(stream_key, stream, message, subscribers, callbacks);
        persist_state_locked();
    }

    for (const auto& subscriber : subscribers) {
        subscriber->send_media(message);
    }

    for (const auto& callback : callbacks) {
        callback(message);
    }
}

void StreamRegistry::upsert_external_stream(
    const std::string& stream_key,
    otts::media::StreamSource source,
    const std::string& audio_codec,
    const std::string& video_codec,
    const std::string& managed_by,
    bool has_publisher) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& stream = streams_[stream_key];
    if (stream.publisher.expired()) {
        stream.source = source;
    }
    stream.ingest_origin = source;
    stream.managed_by = managed_by;
    stream.external_publisher_active = has_publisher;
    stream.audio_track.kind = otts::media::MediaKind::Audio;
    stream.video_track.kind = otts::media::MediaKind::Video;
    stream.audio_track.present = !audio_codec.empty();
    stream.video_track.present = !video_codec.empty();

    auto parse_codec = [](const std::string& value) {
        if (value == "aac") {
            return otts::media::CodecId::Aac;
        }
        if (value == "h264" || value == "avc") {
            return otts::media::CodecId::Avc;
        }
        if (value == "h265" || value == "hevc") {
            return otts::media::CodecId::Hevc;
        }
        if (value == "opus") {
            return otts::media::CodecId::Opus;
        }
        return otts::media::CodecId::Unknown;
    };

    stream.audio_track.codec = parse_codec(audio_codec);
    stream.video_track.codec = parse_codec(video_codec);
    persist_state_locked();
}

void StreamRegistry::update_external_viewers(
    const std::string& stream_key,
    otts::media::StreamSource source,
    const std::string& managed_by,
    std::size_t viewer_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = streams_.find(stream_key);
    auto& stream = streams_[stream_key];
    if (existing == streams_.end() && stream.publisher.expired()) {
        stream.source = source;
        stream.ingest_origin = source;
        stream.managed_by = managed_by;
    }
    stream.external_viewer_count = viewer_count;
    persist_state_locked();
}

void StreamRegistry::upsert_external_session(
    const std::string& session_key,
    const std::string& stream_key,
    otts::media::StreamSource source,
    const std::string& direction,
    const std::string& managed_by,
    const std::string& state,
    const std::string& public_url,
    const std::string& bind_url,
    const std::string& target_url,
    const std::string& transport,
    const std::string& media_path,
    const std::string& native_stage,
    const std::string& codec_hint,
    std::int64_t pid,
    std::uint64_t started_at_epoch_ms,
    std::uint64_t last_stopped_at_epoch_ms,
    std::uint64_t restart_count,
    std::int64_t last_exit_code,
    const std::string& last_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& session = external_sessions_[session_key];
    session.stream_key = stream_key;
    session.source = source;
    session.direction = direction;
    session.managed_by = managed_by;
    session.state = state;
    session.public_url = public_url;
    session.bind_url = bind_url;
    session.target_url = target_url;
    session.transport = transport;
    session.media_path = media_path;
    session.native_stage = native_stage;
    session.codec_hint = codec_hint;
    session.pid = pid;
    session.started_at_epoch_ms = started_at_epoch_ms;
    session.updated_at_epoch_ms = now_epoch_ms();
    session.last_stopped_at_epoch_ms = last_stopped_at_epoch_ms;
    session.restart_count = restart_count;
    session.last_exit_code = last_exit_code;
    session.last_error = last_error;
    persist_state_locked();
}

void StreamRegistry::remove_external_session(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_sessions_.erase(session_key);
    persist_state_locked();
}

void StreamRegistry::remove_external_stream(const std::string& stream_key, otts::media::StreamSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(stream_key);
    if (it == streams_.end()) {
        return;
    }

    if (it->second.source == source) {
        it->second.external_publisher_active = false;
    }

    if (it->second.publisher.expired() && !it->second.external_publisher_active && it->second.subscribers.empty() &&
        it->second.callback_subscribers.empty()) {
        streams_.erase(it);
    }
    persist_state_locked();
}

std::size_t StreamRegistry::viewer_count(const std::string& stream_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_key);
    if (it == streams_.end()) {
        return 0;
    }
    return it->second.subscribers.size();
}

std::vector<StreamRegistry::StreamSnapshot> StreamRegistry::snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamSnapshot> result;
    result.reserve(streams_.size());

    for (const auto& [stream_key, state] : streams_) {
        StreamSnapshot snapshot;
        snapshot.stream_key = stream_key;
        snapshot.source_protocol = otts::media::to_string(state.source);
        snapshot.ingest_origin = otts::media::to_string(state.ingest_origin);
        snapshot.managed_by = state.managed_by;
        snapshot.has_publisher = !state.publisher.expired() || state.external_publisher_active;
        snapshot.viewer_count = state.subscribers.size();
        snapshot.callback_viewer_count = state.callback_subscribers.size();
        snapshot.external_viewer_count = state.external_viewer_count;
        snapshot.has_metadata = state.metadata.has_value();
        snapshot.has_audio_sequence_header = state.audio_sequence_header.has_value();
        snapshot.has_video_sequence_header = state.video_sequence_header.has_value();
        snapshot.audio_codec = otts::media::to_string(state.audio_track.codec);
        snapshot.video_codec = otts::media::to_string(state.video_track.codec);
        snapshot.gop_cache_size = state.gop_cache.size();
        snapshot.total_packets = state.total_packets;
        snapshot.total_bytes = state.total_bytes;
        snapshot.audio_packets = state.audio_packets;
        snapshot.audio_bytes = state.audio_bytes;
        snapshot.video_packets = state.video_packets;
        snapshot.video_bytes = state.video_bytes;
        snapshot.data_packets = state.data_packets;
        snapshot.data_bytes = state.data_bytes;
        snapshot.last_media_timestamp = state.last_media_timestamp;
        snapshot.last_keyframe_at_epoch_ms = state.last_keyframe_at_epoch_ms;
        snapshot.first_media_at_epoch_ms = state.first_media_at_epoch_ms;
        snapshot.last_media_at_epoch_ms = state.last_media_at_epoch_ms;
        result.push_back(std::move(snapshot));
    }

    return result;
}

std::vector<StreamRegistry::ExternalSessionSnapshot> StreamRegistry::external_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ExternalSessionSnapshot> result;
    result.reserve(external_sessions_.size());

    for (const auto& [session_key, state] : external_sessions_) {
        ExternalSessionSnapshot snapshot;
        snapshot.session_key = session_key;
        snapshot.stream_key = state.stream_key;
        snapshot.source_protocol = otts::media::to_string(state.source);
        snapshot.direction = state.direction;
        snapshot.managed_by = state.managed_by;
        snapshot.state = state.state;
        snapshot.public_url = state.public_url;
        snapshot.bind_url = state.bind_url;
        snapshot.target_url = state.target_url;
        snapshot.transport = state.transport;
        snapshot.media_path = state.media_path;
        snapshot.native_stage = state.native_stage;
        snapshot.codec_hint = state.codec_hint;
        snapshot.pid = state.pid;
        snapshot.started_at_epoch_ms = state.started_at_epoch_ms;
        snapshot.updated_at_epoch_ms = state.updated_at_epoch_ms;
        snapshot.last_stopped_at_epoch_ms = state.last_stopped_at_epoch_ms;
        snapshot.restart_count = state.restart_count;
        snapshot.last_exit_code = state.last_exit_code;
        snapshot.last_error = state.last_error;
        result.push_back(std::move(snapshot));
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const ExternalSessionSnapshot& left, const ExternalSessionSnapshot& right) {
            return left.session_key < right.session_key;
        });
    return result;
}

std::optional<StreamRegistry::RtspDescribeInfo> StreamRegistry::rtsp_describe_info(const std::string& stream_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(stream_key);
    if (it == streams_.end() || !it->second.video_sequence_header.has_value()) {
        return std::nullopt;
    }

    const auto& payload = it->second.video_sequence_header->payload;
    if (payload.size() < 13 || payload[1] != 0) {
        return std::nullopt;
    }

    std::size_t offset = 5;
    if (offset + 6 > payload.size()) {
        return std::nullopt;
    }

    RtspDescribeInfo info;
    info.stream_key = stream_key;
    info.video_codec = "h264";

    std::ostringstream profile;
    profile << std::hex;
    profile.width(2);
    profile.fill('0');
    profile << static_cast<int>(payload[offset + 1]);
    profile.width(2);
    profile << static_cast<int>(payload[offset + 2]);
    profile.width(2);
    profile << static_cast<int>(payload[offset + 3]);
    info.profile_level_id = profile.str();

    const auto sps_count = static_cast<std::uint8_t>(payload[offset + 5] & 0x1F);
    offset += 6;
    if (sps_count == 0 || offset + 2 > payload.size()) {
        return std::nullopt;
    }

    const auto sps_length = static_cast<std::size_t>((payload[offset] << 8) | payload[offset + 1]);
    offset += 2;
    if (offset + sps_length > payload.size()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> sps(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                  payload.begin() + static_cast<std::ptrdiff_t>(offset + sps_length));
    offset += sps_length;

    if (offset + 1 > payload.size()) {
        return std::nullopt;
    }
    const auto pps_count = payload[offset];
    offset += 1;
    if (pps_count == 0 || offset + 2 > payload.size()) {
        return std::nullopt;
    }

    const auto pps_length = static_cast<std::size_t>((payload[offset] << 8) | payload[offset + 1]);
    offset += 2;
    if (offset + pps_length > payload.size()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> pps(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                  payload.begin() + static_cast<std::ptrdiff_t>(offset + pps_length));

    info.sprop_parameter_sets = base64_encode(sps) + "," + base64_encode(pps);

    if (it->second.audio_sequence_header.has_value()) {
        const auto& audio_payload = it->second.audio_sequence_header->payload;
        if (audio_payload.size() >= 4 && ((audio_payload[0] >> 4) & 0x0F) == 10 && audio_payload[1] == 0) {
            std::vector<std::uint8_t> config(audio_payload.begin() + 2, audio_payload.end());
            const auto sampling_index = static_cast<std::uint8_t>(((config[0] & 0x07) << 1) | ((config[1] >> 7) & 0x01));
            const auto channels = static_cast<std::uint8_t>((config[1] >> 3) & 0x0F);
            const auto sample_rate = aac_sample_rate_from_index(sampling_index);
            if (sample_rate > 0 && channels > 0) {
                info.has_audio = true;
                info.audio_codec = "aac";
                info.audio_config = hex_encode(config);
                info.audio_sample_rate = sample_rate;
                info.audio_channels = channels;
            }
        }
    }
    return info;
}

bool StreamRegistry::disconnect_stream(const std::string& stream_key) {
    std::vector<std::shared_ptr<RtmpSession>> sessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_key);
        if (it == streams_.end()) {
            return false;
        }

        if (auto publisher = it->second.publisher.lock()) {
            sessions.push_back(std::move(publisher));
        }

        for (const auto& weak : it->second.subscribers) {
            if (auto session = weak.lock()) {
                sessions.push_back(std::move(session));
            }
        }
    }

    for (const auto& session : sessions) {
        session->stop();
    }

    return !sessions.empty();
}

void StreamRegistry::publish_media_locked(
    const std::string& stream_key,
    StreamState& stream,
    const MediaMessage& message,
    std::vector<std::shared_ptr<RtmpSession>>& subscribers,
    std::vector<MediaCallback>& callbacks) {
    const auto packet = to_media_packet(message);
    const auto payload_size = static_cast<std::uint64_t>(message.payload.size());
    const auto now_ms = now_epoch_ms();

    stream.total_packets += 1;
    stream.total_bytes += payload_size;
    stream.last_media_timestamp = message.timestamp;
    if (stream.first_media_at_epoch_ms == 0) {
        stream.first_media_at_epoch_ms = now_ms;
    }
    stream.last_media_at_epoch_ms = now_ms;

    if (message.type_id == 18) {
        stream.metadata = message;
        stream.data_packets += 1;
        stream.data_bytes += payload_size;
        otts::core::log_info("stream_registry", "cached metadata key=" + stream_key);
    } else if (packet.kind == otts::media::MediaKind::Audio) {
        stream.audio_track.present = true;
        stream.audio_track.codec = packet.codec;
        stream.audio_track.last_timestamp_ms = packet.timestamp_ms;
        stream.audio_track.packets += 1;
        stream.audio_track.bytes += payload_size;
        if (packet.is_sequence_header) {
            stream.audio_track.has_sequence_header = true;
        }
    } else if (packet.kind == otts::media::MediaKind::Video) {
        stream.video_track.present = true;
        stream.video_track.codec = packet.codec;
        stream.video_track.last_timestamp_ms = packet.timestamp_ms;
        stream.video_track.packets += 1;
        stream.video_track.bytes += payload_size;
        if (packet.is_sequence_header) {
            stream.video_track.has_sequence_header = true;
        }
        if (packet.is_keyframe) {
            stream.last_keyframe_at_epoch_ms = now_ms;
        }
    }

    if (is_audio_sequence_header(message)) {
        stream.audio_sequence_header = message;
        otts::core::log_info("stream_registry", "cached audio sequence header key=" + stream_key);
    } else if (is_video_sequence_header(message)) {
        stream.video_sequence_header = message;
        otts::core::log_info("stream_registry", "cached video sequence header key=" + stream_key);
    }

    if (message.type_id == 9 || message.type_id == 8) {
        if (message.type_id == 8) {
            stream.audio_packets += 1;
            stream.audio_bytes += payload_size;
        } else if (message.type_id == 9) {
            stream.video_packets += 1;
            stream.video_bytes += payload_size;
        }
        if (packet.kind == otts::media::MediaKind::Video && packet.is_keyframe) {
            otts::core::log_info("stream_registry", "reset GOP cache at keyframe key=" + stream_key);
        }
        stream.gop_cache.add(packet);
    } else if (message.type_id != 18) {
        stream.data_packets += 1;
        stream.data_bytes += payload_size;
    }

    auto& weak_subscribers = stream.subscribers;
    weak_subscribers.erase(
        std::remove_if(
            weak_subscribers.begin(),
            weak_subscribers.end(),
            [](const std::weak_ptr<RtmpSession>& weak) { return weak.expired(); }),
        weak_subscribers.end());

    for (const auto& weak : weak_subscribers) {
        if (auto locked = weak.lock()) {
            subscribers.push_back(std::move(locked));
        }
    }

    callbacks.reserve(stream.callback_subscribers.size());
    for (const auto& subscriber : stream.callback_subscribers) {
        callbacks.push_back(subscriber.callback);
    }
}

void StreamRegistry::persist_state_locked() const {
    std::ofstream file("/tmp/otts_state.json", std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    file << "{\n";
    file << "  \"updated_at_epoch_ms\": 0,\n";
    file << "  \"streams\": [\n";

    bool first = true;
    for (const auto& [stream_key, state] : streams_) {
        if (!first) {
            file << ",\n";
        }
        first = false;

        file << "    {\n";
        file << "      \"stream_key\": \"" << json_escape(stream_key) << "\",\n";
        file << "      \"source_protocol\": \"" << otts::media::to_string(state.source) << "\",\n";
        file << "      \"ingest_origin\": \"" << otts::media::to_string(state.ingest_origin) << "\",\n";
        file << "      \"managed_by\": \"" << json_escape(state.managed_by) << "\",\n";
        file << "      \"has_publisher\": "
             << ((!state.publisher.expired() || state.external_publisher_active) ? "true" : "false") << ",\n";
        file << "      \"viewer_count\": " << state.subscribers.size() << ",\n";
        file << "      \"external_viewer_count\": " << state.external_viewer_count << ",\n";
        file << "      \"has_metadata\": " << (state.metadata ? "true" : "false") << ",\n";
        file << "      \"has_audio_sequence_header\": " << (state.audio_sequence_header ? "true" : "false") << ",\n";
        file << "      \"has_video_sequence_header\": " << (state.video_sequence_header ? "true" : "false") << ",\n";
        file << "      \"gop_cache_size\": " << state.gop_cache.size() << ",\n";
        file << "      \"audio_codec\": \"" << otts::media::to_string(state.audio_track.codec) << "\",\n";
        file << "      \"video_codec\": \"" << otts::media::to_string(state.video_track.codec) << "\",\n";
        file << "      \"callback_subscriber_count\": " << state.callback_subscribers.size() << ",\n";
        file << "      \"total_packets\": " << state.total_packets << ",\n";
        file << "      \"total_bytes\": " << state.total_bytes << ",\n";
        file << "      \"audio_packets\": " << state.audio_packets << ",\n";
        file << "      \"audio_bytes\": " << state.audio_bytes << ",\n";
        file << "      \"video_packets\": " << state.video_packets << ",\n";
        file << "      \"video_bytes\": " << state.video_bytes << ",\n";
        file << "      \"data_packets\": " << state.data_packets << ",\n";
        file << "      \"data_bytes\": " << state.data_bytes << ",\n";
        file << "      \"last_media_timestamp\": " << state.last_media_timestamp << ",\n";
        file << "      \"last_keyframe_at_epoch_ms\": " << state.last_keyframe_at_epoch_ms << ",\n";
        file << "      \"first_media_at_epoch_ms\": " << state.first_media_at_epoch_ms << ",\n";
        file << "      \"last_media_at_epoch_ms\": " << state.last_media_at_epoch_ms << "\n";
        file << "    }";
    }

    file << "\n  ],\n";
    file << "  \"protocol_sessions\": [\n";

    bool first_session = true;
    for (const auto& [session_key, state] : external_sessions_) {
        if (!first_session) {
            file << ",\n";
        }
        first_session = false;
        file << "    {\n";
        file << "      \"session_key\": \"" << json_escape(session_key) << "\",\n";
        file << "      \"stream_key\": \"" << json_escape(state.stream_key) << "\",\n";
        file << "      \"source_protocol\": \"" << otts::media::to_string(state.source) << "\",\n";
        file << "      \"direction\": \"" << json_escape(state.direction) << "\",\n";
        file << "      \"managed_by\": \"" << json_escape(state.managed_by) << "\",\n";
        file << "      \"state\": \"" << json_escape(state.state) << "\",\n";
        file << "      \"public_url\": \"" << json_escape(state.public_url) << "\",\n";
        file << "      \"bind_url\": \"" << json_escape(state.bind_url) << "\",\n";
        file << "      \"target_url\": \"" << json_escape(state.target_url) << "\",\n";
        file << "      \"transport\": \"" << json_escape(state.transport) << "\",\n";
        file << "      \"media_path\": \"" << json_escape(state.media_path) << "\",\n";
        file << "      \"native_stage\": \"" << json_escape(state.native_stage) << "\",\n";
        file << "      \"codec_hint\": \"" << json_escape(state.codec_hint) << "\",\n";
        file << "      \"pid\": " << state.pid << ",\n";
        file << "      \"started_at_epoch_ms\": " << state.started_at_epoch_ms << ",\n";
        file << "      \"updated_at_epoch_ms\": " << state.updated_at_epoch_ms << ",\n";
        file << "      \"last_stopped_at_epoch_ms\": " << state.last_stopped_at_epoch_ms << ",\n";
        file << "      \"restart_count\": " << state.restart_count << ",\n";
        file << "      \"last_exit_code\": " << state.last_exit_code << ",\n";
        file << "      \"last_error\": \"" << json_escape(state.last_error) << "\"\n";
        file << "    }";
    }

    file << "\n  ]\n";
    file << "}\n";
}

std::string StreamRegistry::json_escape(std::string_view value) {
    std::ostringstream escaped;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                escaped << ch;
                break;
        }
    }
    return escaped.str();
}

bool StreamRegistry::is_video_sequence_header(const MediaMessage& message) {
    return message.type_id == 9 && message.payload.size() >= 2 && (message.payload[0] & 0x0F) == 7 &&
           message.payload[1] == 0;
}

bool StreamRegistry::is_audio_sequence_header(const MediaMessage& message) {
    return message.type_id == 8 && message.payload.size() >= 2 && ((message.payload[0] >> 4) & 0x0F) == 10 &&
           message.payload[1] == 0;
}

bool StreamRegistry::is_video_keyframe(const MediaMessage& message) {
    return message.type_id == 9 && !message.payload.empty() && ((message.payload[0] >> 4) & 0x0F) == 1;
}

otts::media::MediaPacket StreamRegistry::to_media_packet(const MediaMessage& message) {
    otts::media::MediaPacket packet;
    packet.timestamp_ms = message.timestamp;
    packet.message_stream_id = message.message_stream_id;
    packet.payload = message.payload;

    if (message.type_id == 18) {
        packet.kind = otts::media::MediaKind::Metadata;
        return packet;
    }

    if (message.type_id == 8) {
        packet.kind = otts::media::MediaKind::Audio;
        packet.is_sequence_header = is_audio_sequence_header(message);
        if (!message.payload.empty()) {
            const auto sound_format = static_cast<std::uint8_t>((message.payload[0] >> 4) & 0x0F);
            if (sound_format == 10) {
                packet.codec = otts::media::CodecId::Aac;
            } else if (sound_format == 13) {
                packet.codec = otts::media::CodecId::Opus;
            }
        }
        return packet;
    }

    if (message.type_id == 9) {
        packet.kind = otts::media::MediaKind::Video;
        packet.is_sequence_header = is_video_sequence_header(message);
        packet.is_keyframe = is_video_keyframe(message);
        if (!message.payload.empty()) {
            const auto codec_id = static_cast<std::uint8_t>(message.payload[0] & 0x0F);
            if (codec_id == 7) {
                packet.codec = otts::media::CodecId::Avc;
            } else if (codec_id == 12) {
                packet.codec = otts::media::CodecId::Hevc;
            }
        }
        return packet;
    }

    packet.kind = otts::media::MediaKind::Data;
    return packet;
}

}  // namespace otts::rtmp
