#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace otts::webrtc {

enum class SessionDirection {
    Publish,
    Play
};

enum class SessionState {
    Pending,
    AwaitingTransport,
    Closed,
    Failed
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
    std::string last_error;
};

class WebRtcService {
public:
    std::string create_session(SessionDirection direction, const std::string& stream_key, const std::string& offer_sdp);
    std::vector<SessionSnapshot> snapshots() const;
    std::optional<SessionSnapshot> snapshot(const std::string& session_id) const;
    bool close_session(const std::string& session_id);
    bool fail_session(const std::string& session_id, const std::string& error);

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
        std::string last_error;
    };

    static std::string make_session_id();
    static std::uint64_t now_epoch_ms();
    static SessionSnapshot make_snapshot(const SessionStateData& state);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionStateData> sessions_;
};

}  // namespace otts::webrtc
