#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace otts::rtmp {
class StreamRegistry;
}

namespace otts::webrtc {

enum class SessionDirection {
    Publish,
    Play
};

enum class SessionState {
    Pending,
    AwaitingTransport,
    Connected,
    Closed,
    Failed
};

enum class RuntimeMode {
    Gateway,
    Auto,
    Native
};

struct NativeStatus {
    RuntimeMode configured_mode{RuntimeMode::Gateway};
    bool compiled_with_dependency{false};
    bool dependency_ready{false};
    bool peer_factory_ready{false};
    bool media_engine_ready{false};
    std::string dependency_root;
    std::string selected_runtime;
    std::string detail;
};

struct NativeOfferResult {
    bool ok{false};
    std::string session_id;
    std::string answer_sdp;
    std::string error;
};

struct SessionSnapshot {
    std::string session_id;
    std::string stream_key;
    std::string direction;
    std::string state;
    std::size_t offer_size{0};
    std::size_t answer_size{0};
    std::uint64_t created_at_epoch_ms{0};
    std::uint64_t updated_at_epoch_ms{0};
    std::uint64_t video_frames{0};
    std::uint64_t video_bytes{0};
    std::uint64_t audio_frames{0};
    std::uint64_t audio_bytes{0};
    std::string transport_state;
    std::string last_error;
};

class WebRtcService {
public:
    explicit WebRtcService(NativeStatus native_status = {});
    ~WebRtcService();

    void attach_registry(otts::rtmp::StreamRegistry& registry);
    std::string create_session(SessionDirection direction, const std::string& stream_key, const std::string& offer_sdp);
    NativeOfferResult handle_native_offer(
        SessionDirection direction,
        const std::string& stream_key,
        const std::string& offer_sdp);
    NativeOfferResult create_native_play_offer(const std::string& stream_key);
    bool set_native_answer(const std::string& session_id, const std::string& answer_sdp);
    std::vector<SessionSnapshot> snapshots() const;
    std::optional<SessionSnapshot> snapshot(const std::string& session_id) const;
    bool close_session(const std::string& session_id);
    bool fail_session(const std::string& session_id, const std::string& error);
    std::size_t cleanup_stale_sessions(std::uint64_t terminal_session_retention_ms);
    NativeStatus native_status() const;
    bool should_use_gateway() const;
    bool requires_native_http_answer() const;
    std::string native_unavailable_json() const;

private:
    struct SessionStateData {
        std::string session_id;
        std::string stream_key;
        SessionDirection direction{SessionDirection::Publish};
        SessionState state{SessionState::Pending};
        std::string offer_sdp;
        std::string answer_sdp;
        std::uint64_t created_at_epoch_ms{0};
        std::uint64_t updated_at_epoch_ms{0};
        std::string transport_state;
        std::string last_error;
    };

    static std::string make_session_id();
    static std::uint64_t now_epoch_ms();
    static SessionSnapshot make_snapshot(
        const SessionStateData& state,
        std::uint64_t video_frames = 0,
        std::uint64_t video_bytes = 0,
        std::uint64_t audio_frames = 0,
        std::uint64_t audio_bytes = 0);
    static std::string mode_to_string(RuntimeMode mode);
    void update_session_state(
        const std::string& session_id,
        SessionState state,
        const std::string& transport_state,
        const std::string& error = {});
    NativeOfferResult handle_native_offer_locked(
        SessionDirection direction,
        const std::string& stream_key,
        const std::string& offer_sdp);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionStateData> sessions_;
    NativeStatus native_status_;
    otts::rtmp::StreamRegistry* registry_{nullptr};

    struct NativeSession;
    std::unordered_map<std::string, std::shared_ptr<NativeSession>> native_sessions_;
};

}  // namespace otts::webrtc
