#include "otts/webrtc/webrtc_service.hpp"

#include <chrono>
#include <random>
#include <sstream>

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
        case SessionState::Closed:
            return "closed";
        case SessionState::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

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

std::vector<SessionSnapshot> WebRtcService::snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSnapshot> result;
    result.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        result.push_back(make_snapshot(session));
    }
    return result;
}

std::optional<SessionSnapshot> WebRtcService::snapshot(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return make_snapshot(it->second);
}

bool WebRtcService::close_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.state = SessionState::Closed;
    it->second.updated_at_epoch_ms = now_epoch_ms();
    return true;
}

bool WebRtcService::fail_session(const std::string& session_id, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.state = SessionState::Failed;
    it->second.last_error = error;
    it->second.updated_at_epoch_ms = now_epoch_ms();
    return true;
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

SessionSnapshot WebRtcService::make_snapshot(const SessionStateData& state) {
    SessionSnapshot snapshot;
    snapshot.session_id = state.session_id;
    snapshot.stream_key = state.stream_key;
    snapshot.direction = direction_to_string(state.direction);
    snapshot.state = state_to_string(state.state);
    snapshot.offer_size = state.offer_sdp.size();
    snapshot.answer_size = state.answer_sdp.size();
    snapshot.created_at_epoch_ms = state.created_at_epoch_ms;
    snapshot.updated_at_epoch_ms = state.updated_at_epoch_ms;
    snapshot.last_error = state.last_error;
    return snapshot;
}

}  // namespace otts::webrtc
