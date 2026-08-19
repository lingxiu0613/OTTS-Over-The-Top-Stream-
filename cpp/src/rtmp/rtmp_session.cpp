#include "otts/rtmp/rtmp_session.hpp"

#include "otts/core/logger.hpp"
#include "otts/rtmp/amf0.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <netinet/tcp.h>
#include <random>
#include <sstream>
#include <thread>

namespace otts::rtmp {

namespace {

constexpr std::uint8_t kRtmpVersion = 3;
constexpr std::uint8_t kMessageSetChunkSize = 1;
constexpr std::uint8_t kMessageAbort = 2;
constexpr std::uint8_t kMessageAck = 3;
constexpr std::uint8_t kMessageUserControl = 4;
constexpr std::uint8_t kMessageWindowAckSize = 5;
constexpr std::uint8_t kMessageSetPeerBandwidth = 6;
constexpr std::uint8_t kMessageAudio = 8;
constexpr std::uint8_t kMessageVideo = 9;
constexpr std::uint8_t kMessageAmf0Data = 18;
constexpr std::uint8_t kMessageAmf0Command = 20;

constexpr std::uint16_t kUserControlStreamBegin = 0;
constexpr std::uint16_t kUserControlPingRequest = 6;
constexpr std::uint16_t kUserControlPingResponse = 7;
constexpr std::size_t kMaxOutboundQueueBytes = 8 * 1024 * 1024;

ssize_t send_without_sigpipe(int socket_fd, const void* data, std::size_t size, int flags = 0) {
#ifdef MSG_NOSIGNAL
    return ::send(socket_fd, data, size, flags | MSG_NOSIGNAL);
#else
    return ::send(socket_fd, data, size, flags);
#endif
}

std::string join_stream_key(const std::string& app_name, const std::string& stream_name) {
    auto trim_slashes = [](std::string value) {
        while (!value.empty() && value.front() == '/') {
            value.erase(value.begin());
        }
        while (!value.empty() && value.back() == '/') {
            value.pop_back();
        }
        return value;
    };

    const std::string app = trim_slashes(app_name);
    const std::string stream = trim_slashes(stream_name);

    if (app.empty()) {
        return stream;
    }

    if (stream.rfind(app + "/", 0) == 0) {
        return stream;
    }

    if (stream.empty()) {
        return app;
    }

    return app + "/" + stream;
}

std::string get_play_stream_name(const std::vector<Amf0Value>& values) {
    for (std::size_t i = 3; i < values.size(); ++i) {
        const auto candidate = as_string(values[i]);
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return {};
}

std::string get_publish_stream_name(const std::vector<Amf0Value>& values) {
    if (values.size() > 3) {
        return as_string(values[3]);
    }
    return {};
}

bool as_bool_value(const Amf0Value& value) {
    if (const auto* b = std::get_if<bool>(&value)) {
        return *b;
    }
    if (const auto* n = std::get_if<double>(&value)) {
        return *n != 0.0;
    }
    if (const auto* s = std::get_if<std::string>(&value)) {
        return *s == "true" || *s == "1";
    }
    return false;
}

std::pair<std::string, std::string> normalize_publish_target(
    const std::string& app_name,
    const std::string& stream_name) {
    const auto slash_pos = app_name.find('/');
    if (slash_pos == std::string::npos) {
        return {app_name, stream_name};
    }

    const auto app = app_name.substr(0, slash_pos);
    const auto suffix = app_name.substr(slash_pos + 1);

    // OBS may send app as "live/stream" and publish type as the last argument.
    if (stream_name.empty() || stream_name == "live" || stream_name == "record" || stream_name == "append") {
        return {app, suffix};
    }

    return {app, stream_name};
}

}  // namespace

RtmpSession::RtmpSession(int socket_fd, std::string client_ip, StreamRegistry& registry)
    : socket_fd_(socket_fd), client_ip_(std::move(client_ip)), registry_(registry) {
    // RTMP players may legitimately send no control message for a long time.
    // TCP keepalive detects dead peers without turning an idle player into a
    // false disconnect after a fixed receive timeout.
    int keepalive = 1;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    int keepidle = 60;
    int keepintvl = 15;
    int keepcnt = 4;
    ::setsockopt(socket_fd_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    ::setsockopt(socket_fd_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    ::setsockopt(socket_fd_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    timeval send_timeout{};
    send_timeout.tv_sec = 5;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
}

RtmpSession::~RtmpSession() {
    running_.store(false);
    if (socket_fd_ >= 0) {
        ::shutdown(socket_fd_, SHUT_RDWR);
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

void RtmpSession::start() {
    running_.store(true);
    std::thread([self = shared_from_this()] { self->outbound_loop(); }).detach();
    std::thread([self = shared_from_this()] {
        if (!self->perform_handshake()) {
            otts::core::log_warn("rtmp_session", "handshake failed for " + self->client_ip_);
            self->stop();
            return;
        }
        self->session_loop();
    }).detach();
}

void RtmpSession::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (auto self = weak_from_this().lock()) {
        registry_.unregister_publisher(self);
        registry_.remove_subscriber(self);
    }

    if (socket_fd_ >= 0) {
        ::shutdown(socket_fd_, SHUT_RDWR);
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    outbound_cv_.notify_all();

    otts::core::log_info("rtmp_session", "closed " + client_ip_);
}

bool RtmpSession::perform_handshake() {
    std::array<std::uint8_t, 1537> c0c1{};
    if (!read_exact(c0c1.data(), c0c1.size())) {
        return false;
    }

    if (c0c1[0] != kRtmpVersion) {
        otts::core::log_warn("rtmp_session", "unsupported RTMP version from " + client_ip_);
        return false;
    }

    std::vector<std::uint8_t> s0s1s2;
    s0s1s2.reserve(1 + 1536 + 1536);
    s0s1s2.push_back(kRtmpVersion);

    std::array<std::uint8_t, 1536> s1{};
    std::mt19937 rng(std::random_device{}());
    for (auto& byte : s1) {
        byte = static_cast<std::uint8_t>(rng() & 0xFF);
    }
    s1[0] = 0;
    s1[1] = 0;
    s1[2] = 0;
    s1[3] = 0;
    s1[4] = 0;
    s1[5] = 0;
    s1[6] = 0;
    s1[7] = 0;

    s0s1s2.insert(s0s1s2.end(), s1.begin(), s1.end());
    s0s1s2.insert(s0s1s2.end(), c0c1.begin() + 1, c0c1.end());

    if (!write_all(s0s1s2.data(), s0s1s2.size())) {
        return false;
    }

    std::array<std::uint8_t, 1536> c2{};
    return read_exact(c2.data(), c2.size());
}

void RtmpSession::session_loop() {
    send_window_ack_size();
    send_set_peer_bandwidth();
    send_set_chunk_size(outbound_chunk_size_);

    while (running_.load()) {
        MediaMessage message;
        if (!receive_message(message)) {
            break;
        }
        handle_message(message);
    }

    stop();
}

void RtmpSession::outbound_loop() {
    while (running_.load()) {
        MediaMessage message;
        {
            std::unique_lock<std::mutex> lock(outbound_mutex_);
            outbound_cv_.wait(lock, [&]() { return !running_.load() || !outbound_queue_.empty(); });
            if (!running_.load() && outbound_queue_.empty()) {
                return;
            }
            message = std::move(outbound_queue_.front());
            outbound_queue_.pop_front();
            outbound_queue_bytes_ -= message.payload.size();
        }
        if (!send_chunked_message(6, message)) {
            stop();
            return;
        }
    }
}

bool RtmpSession::receive_message(MediaMessage& message) {
    std::uint8_t basic_header = 0;
    if (!read_exact(&basic_header, 1)) {
        return false;
    }

    const std::uint8_t fmt = (basic_header >> 6) & 0x03;
    std::uint32_t chunk_stream_id = basic_header & 0x3F;
    if (chunk_stream_id == 0) {
        std::uint8_t extended_csid = 0;
        if (!read_exact(&extended_csid, 1)) {
            return false;
        }
        chunk_stream_id = 64u + extended_csid;
    } else if (chunk_stream_id == 1) {
        std::uint8_t extended_csid[2]{};
        if (!read_exact(extended_csid, sizeof(extended_csid))) {
            return false;
        }
        chunk_stream_id = 64u + extended_csid[0] + (static_cast<std::uint32_t>(extended_csid[1]) << 8u);
    }

    auto& state = chunk_states_[chunk_stream_id];
    const bool continuing_message = !state.payload.empty();
    bool read_extended_timestamp = false;

    if (fmt <= 2) {
        std::uint8_t message_header[11]{};
        std::size_t header_size = 0;
        if (fmt == 0) {
            header_size = 11;
        } else if (fmt == 1) {
            header_size = 7;
        } else {
            header_size = 3;
        }

        if (!read_exact(message_header, header_size)) {
            return false;
        }

        if (fmt == 0) {
            const auto timestamp_field = read_be24(message_header);
            state.extended_timestamp = timestamp_field == 0xFFFFFF;
            state.timestamp_is_delta = false;
            state.timestamp = state.extended_timestamp ? 0 : timestamp_field;
            state.timestamp_delta = 0;
            state.message_length = read_be24(message_header + 3);
            state.type_id = message_header[6];
            state.message_stream_id = read_le32(message_header + 7);
            state.payload.clear();
            state.payload.reserve(state.message_length);
            state.header_ready = true;
        } else if (fmt == 1) {
            const auto timestamp_field = read_be24(message_header);
            state.extended_timestamp = timestamp_field == 0xFFFFFF;
            state.timestamp_is_delta = true;
            state.timestamp_delta = state.extended_timestamp ? 0 : timestamp_field;
            if (!state.extended_timestamp) {
                state.timestamp += state.timestamp_delta;
            }
            state.message_length = read_be24(message_header + 3);
            state.type_id = message_header[6];
            state.payload.clear();
            state.payload.reserve(state.message_length);
            state.header_ready = true;
        } else if (fmt == 2) {
            const auto timestamp_field = read_be24(message_header);
            state.extended_timestamp = timestamp_field == 0xFFFFFF;
            state.timestamp_is_delta = true;
            state.timestamp_delta = state.extended_timestamp ? 0 : timestamp_field;
            if (!state.extended_timestamp) {
                state.timestamp += state.timestamp_delta;
            }
            state.payload.clear();
            state.payload.reserve(state.message_length);
            state.header_ready = true;
        }
        read_extended_timestamp = state.extended_timestamp;
    } else if (!state.header_ready) {
        return false;
    } else {
        read_extended_timestamp = state.extended_timestamp;
        if (!continuing_message && state.timestamp_is_delta && !state.extended_timestamp) {
            state.timestamp += state.timestamp_delta;
        }
    }

    if (read_extended_timestamp) {
        std::uint8_t extended_timestamp[4]{};
        if (!read_exact(extended_timestamp, 4)) {
            return false;
        }
        const auto timestamp_value =
            (static_cast<std::uint32_t>(extended_timestamp[0]) << 24u) |
            (static_cast<std::uint32_t>(extended_timestamp[1]) << 16u) |
            (static_cast<std::uint32_t>(extended_timestamp[2]) << 8u) |
            static_cast<std::uint32_t>(extended_timestamp[3]);
        if (fmt == 0) {
            state.timestamp = timestamp_value;
        } else if (fmt <= 2) {
            state.timestamp_delta = timestamp_value;
            state.timestamp += state.timestamp_delta;
        } else if (!continuing_message && state.timestamp_is_delta) {
            state.timestamp_delta = timestamp_value;
            state.timestamp += state.timestamp_delta;
        }
    }

    if (state.message_length > 32 * 1024 * 1024 || state.payload.size() > state.message_length) {
        otts::core::log_warn("rtmp_session", "invalid RTMP message length");
        return false;
    }

    const std::size_t remaining = state.message_length - state.payload.size();
    const std::size_t to_read = std::min<std::size_t>(remaining, inbound_chunk_size_);
    const auto offset = state.payload.size();
    state.payload.resize(offset + to_read);
    if (!read_exact(state.payload.data() + offset, to_read)) {
        return false;
    }

    if (state.payload.size() < state.message_length) {
        return receive_message(message);
    }

    message.timestamp = state.timestamp;
    message.type_id = state.type_id;
    message.message_stream_id = state.message_stream_id;
    message.payload = std::move(state.payload);
    state.payload.clear();
    return true;
}

void RtmpSession::handle_message(const MediaMessage& message) {
    switch (message.type_id) {
        case kMessageSetChunkSize:
        case kMessageAbort:
        case kMessageAck:
        case kMessageUserControl:
        case kMessageWindowAckSize:
        case kMessageSetPeerBandwidth:
            handle_control_message(message);
            break;
        case kMessageAmf0Command:
            handle_command(message);
            break;
        case kMessageAudio:
        case kMessageVideo:
        case kMessageAmf0Data:
            handle_media_message(message);
            break;
        default:
            break;
    }
}

void RtmpSession::handle_command(const MediaMessage& message) {
    Amf0Reader reader(message.payload);
    std::vector<Amf0Value> values;

    while (!reader.eof()) {
        Amf0Value value;
        if (!reader.read_value(value)) {
            return;
        }
        values.push_back(std::move(value));
    }

    if (values.empty()) {
        return;
    }

    std::ostringstream dump;
    dump << "command args:";
    for (std::size_t i = 0; i < values.size(); ++i) {
        dump << " [" << i << "]=";
        if (const auto* s = std::get_if<std::string>(&values[i])) {
            dump << *s;
        } else if (const auto* n = std::get_if<double>(&values[i])) {
            dump << *n;
        } else if (const auto* b = std::get_if<bool>(&values[i])) {
            dump << (*b ? "true" : "false");
        } else if (std::holds_alternative<Amf0Null>(values[i])) {
            dump << "null";
        } else if (std::holds_alternative<Amf0Object>(values[i])) {
            dump << "{object}";
        }
    }

    const auto command = as_string(values[0]);
    otts::core::log_info("rtmp_session", "received command=" + command + " from " + client_ip_);
    otts::core::log_info("rtmp_session", dump.str());
    if (command == "connect") {
        on_connect(values);
    } else if (command == "releaseStream") {
        on_release_stream(values);
    } else if (command == "FCPublish") {
        on_fc_publish(values);
    } else if (command == "FCUnpublish") {
        on_fc_unpublish(values);
    } else if (command == "createStream") {
        on_create_stream(values);
    } else if (command == "getStreamLength") {
        on_get_stream_length(values);
    } else if (command == "publish") {
        on_publish(message, values);
    } else if (command == "play") {
        on_play(message, values);
    } else if (command == "pause") {
        on_pause(message, values);
    } else if (command == "seek") {
        on_seek(message, values);
    } else if (command == "deleteStream") {
        on_delete_stream(values);
    } else if (values.size() > 1) {
        const auto transaction_id = as_number(values[1]);
        if (transaction_id > 0.0) {
            send_simple_result(transaction_id, Amf0Null{});
        }
    }
}

void RtmpSession::handle_control_message(const MediaMessage& message) {
    if (message.type_id == kMessageSetChunkSize && message.payload.size() >= 4) {
        inbound_chunk_size_ = (message.payload[0] << 24) | (message.payload[1] << 16) |
                              (message.payload[2] << 8) | message.payload[3];
        if (inbound_chunk_size_ == 0 || inbound_chunk_size_ > 16 * 1024 * 1024) {
            otts::core::log_warn("rtmp_session", "invalid inbound chunk size");
            running_.store(false);
            return;
        }
        otts::core::log_info("rtmp_session", "updated inbound chunk size to " + std::to_string(inbound_chunk_size_));
        return;
    }

    if (message.type_id == kMessageUserControl && message.payload.size() >= 6) {
        const auto event_type = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(message.payload[0]) << 8u) | message.payload[1]);
        if (event_type == kUserControlPingRequest) {
            MediaMessage response;
            response.type_id = kMessageUserControl;
            response.message_stream_id = 0;
            response.payload.reserve(6);
            response.payload.push_back(static_cast<std::uint8_t>((kUserControlPingResponse >> 8u) & 0xffu));
            response.payload.push_back(static_cast<std::uint8_t>(kUserControlPingResponse & 0xffu));
            response.payload.insert(response.payload.end(), message.payload.begin() + 2, message.payload.begin() + 6);
            send_chunked_message(2, response);
        }
    }
}

void RtmpSession::handle_media_message(const MediaMessage& message) {
    if (!is_publisher_ || stream_key_.empty()) {
        return;
    }
    registry_.publish_media(stream_key_, message);
}

void RtmpSession::on_connect(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 1.0;
    otts::core::log_info("rtmp_session", "handle connect txn=" + std::to_string(transaction_id));

    if (values.size() > 2) {
        if (const auto* object = std::get_if<Amf0Object>(&values[2])) {
            auto it = object->properties.find("app");
            if (it != object->properties.end()) {
                app_name_ = as_string(it->second);
            }

            if (app_name_.empty()) {
                it = object->properties.find("tcUrl");
                if (it != object->properties.end()) {
                    const auto tc_url = as_string(it->second);
                    const auto scheme_pos = tc_url.find("://");
                    const auto path_pos = tc_url.find('/', scheme_pos == std::string::npos ? 0 : scheme_pos + 3);
                    if (path_pos != std::string::npos && path_pos + 1 < tc_url.size()) {
                        app_name_ = tc_url.substr(path_pos + 1);
                    }
                }
            }
        }
    }

    Amf0Object properties;
    properties.properties["fmsVer"] = std::string("FMS/3,5,7,7009");
    properties.properties["capabilities"] = 31.0;

    Amf0Object info;
    info.properties["level"] = std::string("status");
    info.properties["code"] = std::string("NetConnection.Connect.Success");
    info.properties["description"] = std::string("Connection succeeded.");
    info.properties["objectEncoding"] = 0.0;

    send_command_result(transaction_id, properties, info);
}

void RtmpSession::on_release_stream(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 0.0;
    otts::core::log_info("rtmp_session", "handle releaseStream txn=" + std::to_string(transaction_id));
    send_simple_result(transaction_id, Amf0Null{});
}

void RtmpSession::on_fc_publish(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 0.0;
    otts::core::log_info("rtmp_session", "handle FCPublish txn=" + std::to_string(transaction_id));
    send_simple_result(transaction_id, Amf0Null{});
}

void RtmpSession::on_fc_unpublish(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 0.0;
    otts::core::log_info("rtmp_session", "handle FCUnpublish txn=" + std::to_string(transaction_id));
    send_simple_result(transaction_id, Amf0Null{});
}

void RtmpSession::on_create_stream(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 2.0;
    const auto stream_id = static_cast<double>(next_stream_id_++);
    otts::core::log_info("rtmp_session", "handle createStream txn=" + std::to_string(transaction_id));

    Amf0Writer writer;
    writer.write_string("_result");
    writer.write_number(transaction_id);
    writer.write_null();
    writer.write_number(stream_id);

    MediaMessage response;
    response.type_id = kMessageAmf0Command;
    response.message_stream_id = 0;
    response.payload = writer.data();
    send_chunked_message(3, response);
}

void RtmpSession::on_get_stream_length(const std::vector<Amf0Value>& values) {
    const auto transaction_id = values.size() > 1 ? as_number(values[1]) : 0.0;
    otts::core::log_info("rtmp_session", "handle getStreamLength txn=" + std::to_string(transaction_id));
    if (transaction_id > 0.0) {
        send_simple_result(transaction_id, 0.0);
    }
}

void RtmpSession::on_pause(const MediaMessage& message, const std::vector<Amf0Value>& values) {
    const bool pause = values.size() > 3 ? as_bool_value(values[3]) : false;
    otts::core::log_info("rtmp_session", std::string("handle pause pause=") + (pause ? "true" : "false"));
    send_on_status(
        message.message_stream_id,
        pause ? "NetStream.Pause.Notify" : "NetStream.Unpause.Notify",
        pause ? "Paused stream." : "Unpaused stream.");
}

void RtmpSession::on_seek(const MediaMessage& message, const std::vector<Amf0Value>& values) {
    const auto position = values.size() > 3 ? as_number(values[3]) : 0.0;
    otts::core::log_info("rtmp_session", "handle seek position=" + std::to_string(position));
    send_on_status(message.message_stream_id, "NetStream.Seek.Notify", "Seek is accepted for live stream.");
}

void RtmpSession::on_publish(const MediaMessage& message, const std::vector<Amf0Value>& values) {
    const auto raw_stream_name = get_publish_stream_name(values);
    const auto [normalized_app, normalized_stream] = normalize_publish_target(app_name_, raw_stream_name);
    otts::core::log_info(
        "rtmp_session",
        "handle publish app=" + app_name_ + " stream=" + raw_stream_name);
    otts::core::log_info(
        "rtmp_session",
        "resolved publish app=" + normalized_app + " stream=" + normalized_stream);
    stream_key_ = join_stream_key(normalized_app, normalized_stream);
    otts::core::log_info("rtmp_session", "normalized publish key=" + stream_key_);
    is_publisher_ = true;
    registry_.register_publisher(stream_key_, shared_from_this());

    send_on_status(
        message.message_stream_id,
        "NetStream.Publish.Start",
        "Publishing " + stream_key_);

    otts::core::log_info("rtmp_session", "publisher started for " + stream_key_);
}

void RtmpSession::on_play(const MediaMessage& message, const std::vector<Amf0Value>& values) {
    const auto stream_name = get_play_stream_name(values);
    otts::core::log_info(
        "rtmp_session",
        "handle play app=" + app_name_ + " stream=" + stream_name);
    stream_key_ = join_stream_key(app_name_, stream_name);
    otts::core::log_info("rtmp_session", "normalized play key=" + stream_key_);
    is_player_ = true;
    play_stream_id_ = message.message_stream_id;
    registry_.add_subscriber(stream_key_, shared_from_this());

    send_stream_begin(play_stream_id_);
    send_on_status(play_stream_id_, "NetStream.Play.Reset", "Reset stream.");
    send_on_status(play_stream_id_, "NetStream.Play.Start", "Started playing " + stream_key_);

    otts::core::log_info("rtmp_session", "subscriber started for " + stream_key_);
}

void RtmpSession::on_delete_stream(const std::vector<Amf0Value>& values) {
    otts::core::log_info("rtmp_session", "handle deleteStream");
    if (is_publisher_) {
        registry_.unregister_publisher(shared_from_this());
        is_publisher_ = false;
    }
    if (is_player_) {
        registry_.remove_subscriber(shared_from_this());
        is_player_ = false;
    }
    if (values.size() > 1) {
        const auto transaction_id = as_number(values[1]);
        if (transaction_id > 0.0) {
            send_simple_result(transaction_id, Amf0Null{});
        }
    }
}

bool RtmpSession::read_exact(std::uint8_t* buffer, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        const auto bytes = ::recv(socket_fd_, buffer + total, size - total, 0);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(bytes);
    }
    return true;
}

bool RtmpSession::write_all(const std::uint8_t* data, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        const auto bytes = send_without_sigpipe(socket_fd_, data + total, size - total);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(bytes);
    }
    return true;
}

void RtmpSession::send_window_ack_size() {
    MediaMessage message;
    message.type_id = kMessageWindowAckSize;
    message.message_stream_id = 0;
    message.payload = {0x00, 0x4C, 0x4B, 0x40};
    send_chunked_message(2, message);
}

void RtmpSession::send_set_peer_bandwidth() {
    MediaMessage message;
    message.type_id = kMessageSetPeerBandwidth;
    message.message_stream_id = 0;
    message.payload = {0x00, 0x4C, 0x4B, 0x40, 0x02};
    send_chunked_message(2, message);
}

void RtmpSession::send_set_chunk_size(std::uint32_t chunk_size) {
    MediaMessage message;
    message.type_id = kMessageSetChunkSize;
    message.message_stream_id = 0;
    message.payload = {
        static_cast<std::uint8_t>((chunk_size >> 24) & 0xFF),
        static_cast<std::uint8_t>((chunk_size >> 16) & 0xFF),
        static_cast<std::uint8_t>((chunk_size >> 8) & 0xFF),
        static_cast<std::uint8_t>(chunk_size & 0xFF),
    };
    send_chunked_message(2, message);
}

void RtmpSession::send_stream_begin(std::uint32_t stream_id) {
    MediaMessage message;
    message.type_id = kMessageUserControl;
    message.message_stream_id = 0;
    message.payload = {
        static_cast<std::uint8_t>((kUserControlStreamBegin >> 8) & 0xFF),
        static_cast<std::uint8_t>(kUserControlStreamBegin & 0xFF),
        static_cast<std::uint8_t>((stream_id >> 24) & 0xFF),
        static_cast<std::uint8_t>((stream_id >> 16) & 0xFF),
        static_cast<std::uint8_t>((stream_id >> 8) & 0xFF),
        static_cast<std::uint8_t>(stream_id & 0xFF),
    };
    send_chunked_message(2, message);
}

void RtmpSession::send_on_status(std::uint32_t stream_id, const std::string& code, const std::string& description) {
    Amf0Object info;
    info.properties["level"] = std::string("status");
    info.properties["code"] = code;
    info.properties["description"] = description;

    Amf0Writer writer;
    writer.write_string("onStatus");
    writer.write_number(0.0);
    writer.write_null();
    writer.write_object(info);

    MediaMessage message;
    message.type_id = kMessageAmf0Command;
    message.message_stream_id = stream_id;
    message.payload = writer.data();
    send_chunked_message(5, message);
}

void RtmpSession::send_command_result(double transaction_id, const Amf0Object& properties, const Amf0Object& info) {
    Amf0Writer writer;
    writer.write_string("_result");
    writer.write_number(transaction_id);
    writer.write_object(properties);
    writer.write_object(info);

    MediaMessage message;
    message.type_id = kMessageAmf0Command;
    message.message_stream_id = 0;
    message.payload = writer.data();
    send_chunked_message(3, message);
}

void RtmpSession::send_simple_result(double transaction_id, const Amf0Value& value) {
    Amf0Writer writer;
    writer.write_string("_result");
    writer.write_number(transaction_id);
    writer.write_null();

    std::visit(
        [&writer](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, double>) {
                writer.write_number(item);
            } else if constexpr (std::is_same_v<T, bool>) {
                writer.write_boolean(item);
            } else if constexpr (std::is_same_v<T, std::string>) {
                writer.write_string(item);
            } else if constexpr (std::is_same_v<T, Amf0Null>) {
                writer.write_null();
            } else if constexpr (std::is_same_v<T, Amf0Object>) {
                writer.write_object(item);
            }
        },
        value);

    MediaMessage message;
    message.type_id = kMessageAmf0Command;
    message.message_stream_id = 0;
    message.payload = writer.data();
    send_chunked_message(3, message);
}

void RtmpSession::send_media(const MediaMessage& message) {
    if (!running_.load()) {
        return;
    }
    MediaMessage copy = message;
    copy.message_stream_id = play_stream_id_;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        overflow = outbound_queue_bytes_ + copy.payload.size() > kMaxOutboundQueueBytes;
        if (!overflow) {
            outbound_queue_bytes_ += copy.payload.size();
            outbound_queue_.push_back(std::move(copy));
        }
    }
    if (overflow) {
        otts::core::log_warn("rtmp_session", "disconnecting slow player queue_bytes=" + std::to_string(outbound_queue_bytes_));
        stop();
        return;
    }
    outbound_cv_.notify_one();
}

bool RtmpSession::send_chunked_message(std::uint32_t chunk_stream_id, const MediaMessage& message) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    std::vector<std::uint8_t> out;
    out.reserve(16 + message.payload.size());

    std::size_t offset = 0;
    while (offset < message.payload.size() || (message.payload.empty() && offset == 0)) {
        const bool first_chunk = offset == 0;
        const std::uint8_t fmt = first_chunk ? 0 : 3;
        out.push_back(static_cast<std::uint8_t>((fmt << 6) | (chunk_stream_id & 0x3F)));

        if (first_chunk) {
            write_be24(out, std::min(message.timestamp, 0xFFFFFFu));
            write_be24(out, static_cast<std::uint32_t>(message.payload.size()));
            out.push_back(message.type_id);
            write_le32(out, message.message_stream_id);
            if (message.timestamp >= 0xFFFFFF) {
                out.push_back(static_cast<std::uint8_t>((message.timestamp >> 24) & 0xFF));
                out.push_back(static_cast<std::uint8_t>((message.timestamp >> 16) & 0xFF));
                out.push_back(static_cast<std::uint8_t>((message.timestamp >> 8) & 0xFF));
                out.push_back(static_cast<std::uint8_t>(message.timestamp & 0xFF));
            }
        }

        const std::size_t remaining = message.payload.size() - offset;
        const std::size_t to_write = std::min<std::size_t>(remaining, outbound_chunk_size_);
        if (to_write > 0) {
            out.insert(out.end(), message.payload.begin() + offset, message.payload.begin() + offset + to_write);
        }

        offset += to_write;
        if (message.payload.empty()) {
            offset = 1;
        }
    }

    return write_all(out.data(), out.size());
}

std::uint32_t RtmpSession::read_be24(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 16) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           static_cast<std::uint32_t>(data[2]);
}

std::uint32_t RtmpSession::read_le32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[3]) << 24) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           static_cast<std::uint32_t>(data[0]);
}

void RtmpSession::write_be24(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void RtmpSession::write_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

}  // namespace otts::rtmp
