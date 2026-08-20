#pragma once

#include "otts/media/stream_model.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace otts::rtmp {

class RtmpSession;

struct MediaMessage {
    std::uint32_t timestamp{0};
    std::uint8_t type_id{0};
    std::uint32_t message_stream_id{1};
    std::vector<std::uint8_t> payload;
};

class StreamRegistry {
public:
    using CallbackId = std::uint64_t;
    using MediaCallback = std::function<void(const MediaMessage&)>;

    struct StreamSnapshot {
        std::string stream_key;
        std::string source_protocol;
        std::string ingest_origin;
        std::string managed_by;
        bool has_publisher{false};
        std::size_t viewer_count{0};
        std::size_t callback_viewer_count{0};
        std::size_t external_viewer_count{0};
        bool has_metadata{false};
        bool has_audio_sequence_header{false};
        bool has_video_sequence_header{false};
        bool has_keyframe{false};
        bool ready_for_play{false};
        std::string audio_codec;
        std::string video_codec;
        std::size_t track_count{0};
        std::size_t gop_cache_size{0};
        std::uint64_t total_packets{0};
        std::uint64_t total_bytes{0};
        double average_packet_rate{0.0};
        double average_bitrate_kbps{0.0};
        std::uint64_t audio_packets{0};
        std::uint64_t audio_bytes{0};
        std::uint64_t video_packets{0};
        std::uint64_t video_bytes{0};
        std::uint64_t data_packets{0};
        std::uint64_t data_bytes{0};
        std::uint32_t last_media_timestamp{0};
        std::uint64_t last_keyframe_at_epoch_ms{0};
        std::uint64_t first_media_at_epoch_ms{0};
        std::uint64_t last_media_at_epoch_ms{0};
        std::uint64_t last_media_age_ms{0};
        std::uint64_t publish_generation{0};
    };

    struct RtspDescribeInfo {
        std::string stream_key;
        std::string video_codec;
        std::string profile_level_id;
        std::string sprop_parameter_sets;
        bool has_audio{false};
        std::string audio_codec;
        std::string audio_config;
        std::uint32_t audio_sample_rate{0};
        std::uint8_t audio_channels{0};
    };

    struct ExternalSessionSnapshot {
        std::string session_key;
        std::string stream_key;
        std::string source_protocol;
        std::string direction;
        std::string managed_by;
        std::string state;
        std::string public_url;
        std::string bind_url;
        std::string target_url;
        std::string transport;
        std::string media_path;
        std::string native_stage;
        std::string codec_hint;
        std::int64_t pid{0};
        std::uint64_t started_at_epoch_ms{0};
        std::uint64_t updated_at_epoch_ms{0};
        std::uint64_t last_stopped_at_epoch_ms{0};
        std::uint64_t restart_count{0};
        std::int64_t last_exit_code{0};
        std::string last_error;
    };

    struct CleanupStats {
        std::uint64_t runs{0};
        std::uint64_t expired_subscribers{0};
        std::uint64_t inactive_external_publishers{0};
        std::uint64_t removed_streams{0};
        std::uint64_t removed_external_sessions{0};
        std::uint64_t last_run_epoch_ms{0};
    };

    void register_publisher(const std::string& stream_key, const std::shared_ptr<RtmpSession>& session);
    void unregister_publisher(const std::shared_ptr<RtmpSession>& session);
    void add_subscriber(const std::string& stream_key, const std::shared_ptr<RtmpSession>& session);
    void remove_subscriber(const std::shared_ptr<RtmpSession>& session);
    void add_callback_subscriber(const std::string& stream_key, CallbackId callback_id, MediaCallback callback);
    void add_live_callback_subscriber(const std::string& stream_key, CallbackId callback_id, MediaCallback callback);
    void remove_callback_subscriber(const std::string& stream_key, CallbackId callback_id);
    void publish_media(const std::string& stream_key, const MediaMessage& message);
    void publish_external_media(
        const std::string& stream_key,
        otts::media::StreamSource source,
        const std::string& managed_by,
        const MediaMessage& message);
    void begin_external_publish(
        const std::string& stream_key,
        otts::media::StreamSource source,
        const std::string& managed_by);
    void upsert_external_stream(
        const std::string& stream_key,
        otts::media::StreamSource source,
        const std::string& audio_codec,
        const std::string& video_codec,
        const std::string& managed_by,
        bool has_publisher);
    void update_external_viewers(
        const std::string& stream_key,
        otts::media::StreamSource source,
        const std::string& managed_by,
        std::size_t viewer_count);
    void upsert_external_session(
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
        const std::string& last_error);
    void remove_external_session(const std::string& session_key);
    void remove_external_stream(const std::string& stream_key, otts::media::StreamSource source);
    std::size_t viewer_count(const std::string& stream_key);
    std::vector<StreamSnapshot> snapshots() const;
    std::vector<ExternalSessionSnapshot> external_sessions() const;
    std::vector<MediaMessage> cached_messages(const std::string& stream_key) const;
    CleanupStats cleanup_stale(std::uint64_t external_publisher_idle_ms, std::uint64_t stopped_session_retention_ms);
    CleanupStats cleanup_stats() const;
    std::optional<RtspDescribeInfo> rtsp_describe_info(const std::string& stream_key) const;
    bool disconnect_stream(const std::string& stream_key);

private:
    struct CallbackSubscriber {
        CallbackId id{0};
        MediaCallback callback;
    };

    struct StreamState;

    void persist_state_locked() const;
    void persist_media_state_if_due_locked();
    static std::string json_escape(std::string_view value);
    static bool is_video_sequence_header(const MediaMessage& message);
    static bool is_audio_sequence_header(const MediaMessage& message);
    static bool is_video_keyframe(const MediaMessage& message);
    static otts::media::MediaPacket to_media_packet(const MediaMessage& message);
    static std::vector<MediaMessage> snapshot_cached_messages_locked(const StreamState& stream);
    void publish_media_locked(
        const std::string& stream_key,
        StreamState& stream,
        const MediaMessage& message,
        std::vector<std::shared_ptr<RtmpSession>>& subscribers,
        std::vector<MediaCallback>& callbacks);

    struct StreamState {
        std::weak_ptr<RtmpSession> publisher;
        otts::media::StreamSource source{otts::media::StreamSource::Unknown};
        otts::media::StreamSource ingest_origin{otts::media::StreamSource::Unknown};
        std::string managed_by;
        bool external_publisher_active{false};
        std::vector<std::weak_ptr<RtmpSession>> subscribers;
        std::optional<MediaMessage> metadata;
        std::optional<MediaMessage> audio_sequence_header;
        std::optional<MediaMessage> video_sequence_header;
        otts::media::TrackState audio_track{};
        otts::media::TrackState video_track{};
        otts::media::GopCache gop_cache;
        std::vector<CallbackSubscriber> callback_subscribers;
        std::size_t external_viewer_count{0};
        std::uint64_t total_packets{0};
        std::uint64_t total_bytes{0};
        std::uint64_t audio_packets{0};
        std::uint64_t audio_bytes{0};
        std::uint64_t video_packets{0};
        std::uint64_t video_bytes{0};
        std::uint64_t data_packets{0};
        std::uint64_t data_bytes{0};
        std::uint32_t last_media_timestamp{0};
        std::uint64_t last_keyframe_at_epoch_ms{0};
        std::uint64_t first_media_at_epoch_ms{0};
        std::uint64_t last_media_at_epoch_ms{0};
        std::uint64_t publish_generation{0};
    };

    struct ExternalSessionState {
        std::string stream_key;
        otts::media::StreamSource source{otts::media::StreamSource::Unknown};
        std::string direction;
        std::string managed_by;
        std::string state;
        std::string public_url;
        std::string bind_url;
        std::string target_url;
        std::string transport;
        std::string media_path;
        std::string native_stage;
        std::string codec_hint;
        std::int64_t pid{0};
        std::uint64_t started_at_epoch_ms{0};
        std::uint64_t updated_at_epoch_ms{0};
        std::uint64_t last_stopped_at_epoch_ms{0};
        std::uint64_t restart_count{0};
        std::int64_t last_exit_code{0};
        std::string last_error;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamState> streams_;
    std::unordered_map<std::string, ExternalSessionState> external_sessions_;
    CleanupStats cleanup_stats_;
    std::uint64_t last_media_state_persist_epoch_ms_{0};
};

}  // namespace otts::rtmp
