
#include "otts/rtsp/rtsp_play_server.hpp"

#include "otts/auth/stream_auth.hpp"
#include "otts/codec/video_codec.hpp"

#include "otts/core/logger.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace otts::rtsp {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}


std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string query_value(const std::string& uri, const std::string& key) {
    const auto query = uri.find('?');
    if (query == std::string::npos) {
        return {};
    }
    std::size_t cursor = query + 1;
    while (cursor < uri.size()) {
        const auto next = uri.find('&', cursor);
        const auto part = uri.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        const auto equal = part.find('=');
        if (equal != std::string::npos && part.substr(0, equal) == key) {
            return part.substr(equal + 1);
        }
        if (next == std::string::npos) {
            break;
        }
        cursor = next + 1;
    }
    return {};
}

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
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

std::string stream_key_from_uri(std::string uri) {
    const auto query = uri.find('?');
    if (query != std::string::npos) {
        uri = uri.substr(0, query);
    }
    const auto scheme = uri.find("://");
    if (scheme != std::string::npos) {
        const auto slash = uri.find('/', scheme + 3);
        uri = slash == std::string::npos ? std::string{} : uri.substr(slash + 1);
    }
    while (!uri.empty() && uri.front() == '/') {
        uri.erase(uri.begin());
    }
    if (uri.size() > 4 && uri.substr(uri.size() - 4) == ".sdp") {
        uri.resize(uri.size() - 4);
    }
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (i + 1 < uri.size() && uri[i] == '_' && uri[i + 1] == '_') {
            out.push_back('/');
            ++i;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

struct RtspRequest {
    std::string method;
    std::string uri;
    std::unordered_map<std::string, std::string> headers;
};

std::optional<RtspRequest> parse_request(const std::string& raw) {
    std::istringstream stream(raw);
    RtspRequest request;
    std::string version;
    stream >> request.method >> request.uri >> version;
    if (request.method.empty()) {
        return std::nullopt;
    }
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return request;
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_client_ports(const std::string& transport) {
    const auto key = transport.find("client_port=");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const auto start = key + std::string("client_port=").size();
    const auto dash = transport.find('-', start);
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    try {
        const auto rtp = static_cast<std::uint16_t>(std::stoul(transport.substr(start, dash - start)));
        const auto rtcp = static_cast<std::uint16_t>(std::stoul(transport.substr(dash + 1)));
        return std::make_pair(rtp, rtcp);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> parse_interleaved_channels(const std::string& transport) {
    const auto key = transport.find("interleaved=");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const auto start = key + std::string("interleaved=").size();
    const auto dash = transport.find('-', start);
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    try {
        const auto rtp = static_cast<std::uint8_t>(std::stoul(transport.substr(start, dash - start)));
        const auto rtcp = static_cast<std::uint8_t>(std::stoul(transport.substr(dash + 1)));
        return std::make_pair(rtp, rtcp);
    } catch (...) {
        return std::nullopt;
    }
}

std::string response_text(
    const std::string& status,
    const std::string& cseq,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    const std::string& body = {}) {
    std::ostringstream response;
    response << "RTSP/1.0 " << status << "\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: OTTS-CXX-RTSP/0.1\r\n";
    for (const auto& [key, value] : headers) {
        response << key << ": " << value << "\r\n";
    }
    if (!body.empty()) {
        response << "Content-Length: " << body.size() << "\r\n";
    }
    response << "\r\n";
    response << body;
    return response.str();
}


class RtpTrackSender {
public:
    bool open(const std::string& client_ip, std::uint16_t client_port, std::uint8_t payload_type, std::uint32_t clock_rate) {
        payload_type_ = payload_type;
        clock_rate_ = clock_rate;
        socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(local);
        if (::getsockname(socket_fd_, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            local_port_ = ntohs(local.sin_port);
        }
        remote_.sin_family = AF_INET;
        remote_.sin_port = htons(client_port);
        if (::inet_pton(AF_INET, client_ip.c_str(), &remote_.sin_addr) != 1) {
            ::close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        std::random_device rd;
        ssrc_ = rd();
        sequence_ = static_cast<std::uint16_t>(rd());
        return true;
    }

    bool open_tcp(int control_fd, std::uint8_t interleaved_channel, std::uint8_t payload_type, std::uint32_t clock_rate) {
        control_fd_ = control_fd;
        interleaved_channel_ = interleaved_channel;
        payload_type_ = payload_type;
        clock_rate_ = clock_rate;
        use_tcp_ = true;
        std::random_device rd;
        ssrc_ = rd();
        sequence_ = static_cast<std::uint16_t>(rd());
        return true;
    }

    void close() {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        use_tcp_ = false;
        control_fd_ = -1;
    }

    std::uint16_t local_port() const { return local_port_; }

    void send_flv_video(const otts::rtmp::MediaMessage& message) {
        if ((!use_tcp_ && socket_fd_ < 0) || message.type_id != 9 || message.payload.size() < 5) {
            return;
        }
        const auto video = otts::codec::parse_flv_video_packet(message.payload);
        if (!video || !video->coded_frames) {
            return;
        }
        const auto present_time_ms = static_cast<std::int64_t>(message.timestamp) + video->composition_time_ms;
        const auto normalized_present_time_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(0, present_time_ms));
        // RTP video timestamps represent presentation time. Do not synthesize a
        // fixed frame delta here: B-frame decode order makes PTS non-monotonic,
        // and replacing those values changes 25 fps input into roughly 30 fps.
        const auto rtp_timestamp = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(normalized_present_time_ms) * clock_rate_) / 1000);
        const auto annexb = otts::codec::flv_video_sample_to_annexb(message.payload);
        std::size_t cursor = 0;
        while (cursor + 3 < annexb.size()) {
            std::size_t code = 0;
            if (annexb[cursor] == 0 && annexb[cursor + 1] == 0 && annexb[cursor + 2] == 1) code = 3;
            else if (annexb[cursor] == 0 && annexb[cursor + 1] == 0 && annexb[cursor + 2] == 0 && annexb[cursor + 3] == 1) code = 4;
            if (code == 0) { ++cursor; continue; }
            const auto begin = cursor + code;
            auto end = begin;
            while (end + 3 < annexb.size() &&
                   !(annexb[end] == 0 && annexb[end + 1] == 0 &&
                     (annexb[end + 2] == 1 || (annexb[end + 2] == 0 && annexb[end + 3] == 1)))) ++end;
            if (end + 3 >= annexb.size()) end = annexb.size();
            if (end > begin) send_nal(annexb.data() + begin, end - begin, rtp_timestamp, end == annexb.size(), video->codec);
            cursor = end;
        }
    }

    void send_flv_aac(const otts::rtmp::MediaMessage& message) {
        if ((!use_tcp_ && socket_fd_ < 0) || message.type_id != 8 || message.payload.size() <= 2) {
            return;
        }
        const auto sound_format = static_cast<std::uint8_t>((message.payload[0] >> 4) & 0x0f);
        const auto aac_packet_type = message.payload[1];
        if (sound_format != 10 || aac_packet_type != 1) {
            return;
        }
        const auto raw_size = message.payload.size() - 2;
        if (raw_size == 0 || raw_size > 8191) {
            return;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(4 + raw_size);
        write_be16(payload, 16);
        // RFC 3640 AU-size is expressed in bytes; the low three bits carry AU-Index.
        const auto au_header = static_cast<std::uint16_t>(raw_size << 3);
        write_be16(payload, au_header);
        payload.insert(payload.end(), message.payload.begin() + 2, message.payload.end());
        const auto rtp_timestamp = next_aac_rtp_timestamp(message.timestamp);
        send_packet(payload.data(), payload.size(), rtp_timestamp, true);
    }

private:
    std::uint32_t next_aac_rtp_timestamp(std::uint32_t input_timestamp_ms) {
        if (!rtp_timestamp_accumulator_.has_value()) {
            last_input_timestamp_ms_ = input_timestamp_ms;
            rtp_timestamp_accumulator_ = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(input_timestamp_ms) * clock_rate_) / 1000);
            return *rtp_timestamp_accumulator_;
        }
        last_input_timestamp_ms_ = input_timestamp_ms;
        *rtp_timestamp_accumulator_ += 1024;
        return *rtp_timestamp_accumulator_;
    }

    void send_packet(const std::uint8_t* payload, std::size_t size, std::uint32_t timestamp, bool marker) {
        std::vector<std::uint8_t> packet;
        packet.reserve(12 + size);
        packet.push_back(0x80);
        packet.push_back(static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | payload_type_));
        write_be16(packet, sequence_++);
        write_be32(packet, timestamp);
        write_be32(packet, ssrc_);
        packet.insert(packet.end(), payload, payload + size);
        if (use_tcp_) {
            if (packet.size() > 0xffff) {
                return;
            }
            std::vector<std::uint8_t> frame;
            frame.reserve(packet.size() + 4);
            frame.push_back('$');
            frame.push_back(interleaved_channel_);
            write_be16(frame, static_cast<std::uint16_t>(packet.size()));
            frame.insert(frame.end(), packet.begin(), packet.end());
            // A slow TCP-interleaved reader must not block the registry's
            // media fanout thread. Drop the current RTP frame if the kernel
            // send queue is full; the next keyframe lets the reader recover.
            const auto sent = ::send(control_fd_, frame.data(), frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            if (sent != static_cast<ssize_t>(frame.size())) {
                ::shutdown(control_fd_, SHUT_RDWR);
            }
            return;
        }
        ::sendto(socket_fd_, packet.data(), packet.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&remote_), sizeof(remote_));
    }

    void send_nal(
        const std::uint8_t* nal,
        std::size_t size,
        std::uint32_t timestamp,
        bool marker,
        otts::media::CodecId codec) {
        constexpr std::size_t max_payload = 1200;
        if (size <= max_payload) {
            send_packet(nal, size, timestamp, marker);
            return;
        }
        if (codec == otts::media::CodecId::Hevc) {
            if (size < 3) return;
            const auto nal_type = static_cast<std::uint8_t>((nal[0] >> 1) & 0x3f);
            const std::array<std::uint8_t, 2> fu_indicator{
                static_cast<std::uint8_t>((nal[0] & 0x81) | (49 << 1)), nal[1]};
            std::size_t offset = 2;
            bool start = true;
            while (offset < size) {
                const auto chunk = std::min<std::size_t>(size - offset, max_payload - 3);
                const bool end = offset + chunk >= size;
                std::array<std::uint8_t, max_payload> payload{};
                payload[0] = fu_indicator[0];
                payload[1] = fu_indicator[1];
                payload[2] = static_cast<std::uint8_t>((start ? 0x80 : 0) | (end ? 0x40 : 0) | nal_type);
                std::memcpy(payload.data() + 3, nal + offset, chunk);
                send_packet(payload.data(), chunk + 3, timestamp, end && marker);
                start = false;
                offset += chunk;
            }
            return;
        }
        const auto nal_header = nal[0];
        const auto fu_indicator = static_cast<std::uint8_t>((nal_header & 0xe0) | 28);
        const auto nal_type = static_cast<std::uint8_t>(nal_header & 0x1f);
        std::size_t offset = 1;
        bool start = true;
        while (offset < size) {
            const auto remaining = size - offset;
            const auto chunk = std::min<std::size_t>(remaining, max_payload - 2);
            const bool end = offset + chunk >= size;
            std::array<std::uint8_t, max_payload> payload{};
            payload[0] = fu_indicator;
            payload[1] = static_cast<std::uint8_t>((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type);
            std::memcpy(payload.data() + 2, nal + offset, chunk);
            send_packet(payload.data(), chunk + 2, timestamp, end && marker);
            start = false;
            offset += chunk;
        }
    }

    int socket_fd_{-1};
    sockaddr_in remote_{};
    std::uint16_t local_port_{0};
    std::uint16_t sequence_{0};
    std::uint32_t ssrc_{0};
    std::uint8_t payload_type_{96};
    std::uint32_t clock_rate_{90000};
    bool use_tcp_{false};
    int control_fd_{-1};
    std::uint8_t interleaved_channel_{0};
    std::optional<std::uint32_t> last_input_timestamp_ms_;
    std::optional<std::uint32_t> rtp_timestamp_accumulator_;
};

}  // namespace

RtspPlayServer::RtspPlayServer(std::uint16_t port, otts::rtmp::StreamRegistry& registry)
    : port_(port), registry_(registry) {}

RtspPlayServer::~RtspPlayServer() {
    stop();
}

bool RtspPlayServer::start() {
    if (port_ == 0) {
        return true;
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        otts::core::log_error("rtsp_play", std::string("bind failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 32) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    running_.store(true);
    std::thread(&RtspPlayServer::accept_loop, this).detach();
    otts::core::log_info("rtsp_play", "C++ RTSP play listening on 0.0.0.0:" + std::to_string(port_));
    return true;
}

void RtspPlayServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void RtspPlayServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0) {
            continue;
        }
        std::thread(&RtspPlayServer::handle_client, this, fd).detach();
    }
}

void RtspPlayServer::handle_client(int client_fd) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    std::string client_ip = "127.0.0.1";
    if (::getpeername(client_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
        char ip[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) != nullptr) {
            client_ip = ip;
        }
    }

    std::string buffer;
    std::string stream_key;
    std::string session_id = std::to_string(next_callback_id_.fetch_add(1));
    RtpTrackSender video_sender;
    RtpTrackSender audio_sender;
    bool video_setup = false;
    bool audio_setup = false;
    otts::rtmp::StreamRegistry::CallbackId callback_id = 0;
    bool subscribed = false;
    bool authorized = false;
    bool playback_running = true;
    std::string session_key = "cpp-rtsp-play:" + session_id;
    std::mutex sender_mutex;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<otts::rtmp::MediaMessage> pending_messages;
    std::optional<std::uint32_t> media_origin_timestamp_ms;
    std::optional<std::chrono::steady_clock::time_point> playback_wall_origin;
    std::optional<std::chrono::steady_clock::time_point> live_buffer_started_at;
    bool live_playback_started = false;

    auto playback_worker = std::thread([&]() {
        while (true) {
            otts::rtmp::MediaMessage message;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&]() { return !playback_running || !pending_messages.empty(); });
                if (!playback_running && pending_messages.empty()) {
                    break;
                }
                if (!live_playback_started) {
                    if (!live_buffer_started_at.has_value()) {
                        live_buffer_started_at = std::chrono::steady_clock::now();
                    }
                    std::size_t queued_video_messages = 0;
                    for (const auto& queued : pending_messages) {
                        const auto video = queued.type_id == 9
                            ? otts::codec::parse_flv_video_packet(queued.payload)
                            : std::nullopt;
                        if (video && video->coded_frames) {
                            ++queued_video_messages;
                        }
                    }
                    const auto buffered_for = std::chrono::steady_clock::now() - *live_buffer_started_at;
                    if (queued_video_messages < 6 && buffered_for < std::chrono::milliseconds(180)) {
                        queue_cv.wait_for(lock, std::chrono::milliseconds(20));
                        continue;
                    }
                    live_playback_started = true;
                }
                const auto next = std::min_element(
                    pending_messages.begin(),
                    pending_messages.end(),
                    [](const auto& left, const auto& right) { return left.timestamp < right.timestamp; });
                message = std::move(*next);
                pending_messages.erase(next);
            }

            const auto video = message.type_id == 9
                ? otts::codec::parse_flv_video_packet(message.payload)
                : std::nullopt;
            const bool is_raw_video = video && video->coded_frames;
            const bool is_raw_audio = message.type_id == 8 && message.payload.size() > 1 && message.payload[1] == 1;
            if (is_raw_video || is_raw_audio) {
                auto now = std::chrono::steady_clock::now();
                if (!media_origin_timestamp_ms.has_value() || !playback_wall_origin.has_value()) {
                    media_origin_timestamp_ms = message.timestamp;
                    playback_wall_origin = now;
                }
                if (message.timestamp >= *media_origin_timestamp_ms) {
                    auto due = *playback_wall_origin +
                               std::chrono::milliseconds(message.timestamp - *media_origin_timestamp_ms);
                    const auto too_far_ahead = due > now + std::chrono::seconds(2);
                    const auto too_far_behind = due + std::chrono::seconds(2) < now;
                    if (too_far_ahead || too_far_behind) {
                        media_origin_timestamp_ms = message.timestamp;
                        playback_wall_origin = now;
                        due = now;
                    }
                    if (due > now) {
                        std::this_thread::sleep_until(due);
                    }
                }
            }

            std::lock_guard<std::mutex> lock(sender_mutex);
            if (video_setup) {
                video_sender.send_flv_video(message);
            }
            if (audio_setup) {
                audio_sender.send_flv_aac(message);
            }
        }
    });

    auto cleanup = [&]() {
        if (subscribed) {
            registry_.remove_callback_subscriber(stream_key, callback_id);
            subscribed = false;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            playback_running = false;
        }
        queue_cv.notify_all();
        if (playback_worker.joinable()) {
            playback_worker.join();
        }
        registry_.remove_external_session(session_key);
        video_sender.close();
        audio_sender.close();
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
    };

    char recv_buf[4096];
    while (true) {
        const auto n = ::recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0) {
            cleanup();
            return;
        }
        buffer.append(recv_buf, static_cast<std::size_t>(n));
        while (true) {
            const auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                break;
            }
            const auto raw = buffer.substr(0, end + 4);
            buffer.erase(0, end + 4);
            const auto parsed = parse_request(raw);
            if (!parsed.has_value()) {
                cleanup();
                return;
            }
            const auto& request = *parsed;
            const auto cseq_it = request.headers.find("cseq");
            const auto cseq = cseq_it == request.headers.end() ? "1" : cseq_it->second;

            if (request.method == "OPTIONS") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER, TEARDOWN"}}));
                continue;
            }

            if (stream_key.empty()) {
                stream_key = stream_key_from_uri(request.uri);
            }

            if (request.method != "OPTIONS" && !authorized && !stream_key.empty()) {
                authorized = otts::auth::is_authorized(
                    "play",
                    stream_key,
                    query_value(request.uri, "token"),
                    query_value(request.uri, "expires"),
                    query_value(request.uri, "sign"));
                if (!authorized) {
                    send_all(client_fd, response_text("401 Unauthorized", cseq));
                    cleanup();
                    return;
                }
            }

            if (request.method == "DESCRIBE") {
                std::optional<otts::rtmp::StreamRegistry::RtspDescribeInfo> info;
                for (int attempt = 0; attempt < 30; ++attempt) {
                    info = registry_.rtsp_describe_info(stream_key);
                    if (info.has_value()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!info.has_value()) {
                    otts::core::log_warn("rtsp_play", "DESCRIBE stream not ready key=" + stream_key);
                    send_all(client_fd, response_text("404 Not Found", cseq));
                    continue;
                }
                std::ostringstream sdp;
                sdp << "v=0\r\n";
                sdp << "o=- 0 0 IN IP4 0.0.0.0\r\n";
                sdp << "s=OTTS C++ RTSP Play\r\n";
                sdp << "c=IN IP4 0.0.0.0\r\n";
                sdp << "t=0 0\r\n";
                sdp << "m=video 0 RTP/AVP 96\r\n";
                sdp << "a=control:trackID=0\r\n";
                if (info->video_codec == "h265") {
                    std::istringstream values(info->sprop_parameter_sets);
                    std::string vps, sps_value, pps;
                    std::getline(values, vps, ',');
                    std::getline(values, sps_value, ',');
                    std::getline(values, pps, ',');
                    sdp << "a=rtpmap:96 H265/90000\r\n";
                    sdp << "a=fmtp:96 sprop-vps=" << vps << ";sprop-sps=" << sps_value
                        << ";sprop-pps=" << pps << "\r\n";
                } else {
                    sdp << "a=rtpmap:96 H264/90000\r\n";
                    sdp << "a=fmtp:96 packetization-mode=1;profile-level-id=" << info->profile_level_id
                        << ";sprop-parameter-sets=" << info->sprop_parameter_sets << "\r\n";
                }
                if (info->has_audio) {
                    sdp << "m=audio 0 RTP/AVP 97\r\n";
                    sdp << "a=control:trackID=1\r\n";
                    sdp << "a=rtpmap:97 MPEG4-GENERIC/" << info->audio_sample_rate << "/" << info->audio_channels << "\r\n";
                    sdp << "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;config=" << info->audio_config
                        << ";SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n";
                }
                send_all(client_fd, response_text("200 OK", cseq, {{"Content-Type", "application/sdp"}, {"Content-Base", request.uri}}, sdp.str()));
                continue;
            }

            if (request.method == "SETUP") {
                const auto transport_it = request.headers.find("transport");
                const auto transport_header = transport_it == request.headers.end() ? std::string{} : transport_it->second;
                const auto transport_lower = lower(transport_header);
                const bool tcp_interleaved = transport_lower.find("rtp/avp/tcp") != std::string::npos;
                const auto ports = parse_client_ports(transport_header);
                const auto channels = parse_interleaved_channels(transport_header);
                const auto uri_lower = lower(request.uri);
                const bool is_audio_track = uri_lower.find("trackid=1") != std::string::npos;
                std::uint16_t server_rtp = 0;
                std::uint8_t rtp_channel = is_audio_track ? 2 : 0;
                std::uint8_t rtcp_channel = is_audio_track ? 3 : 1;
                if (channels.has_value()) {
                    rtp_channel = channels->first;
                    rtcp_channel = channels->second;
                }
                if (is_audio_track) {
                    auto info = registry_.rtsp_describe_info(stream_key);
                    const auto sample_rate = info && info->audio_sample_rate ? info->audio_sample_rate : 44100;
                    const bool ok = tcp_interleaved
                        ? audio_sender.open_tcp(client_fd, rtp_channel, 97, sample_rate)
                        : (ports.has_value() && audio_sender.open(client_ip, ports->first, 97, sample_rate));
                    if (!ok) {
                        send_all(client_fd, response_text("461 Unsupported Transport", cseq));
                        continue;
                    }
                    audio_setup = true;
                    server_rtp = audio_sender.local_port();
                } else {
                    const bool ok = tcp_interleaved
                        ? video_sender.open_tcp(client_fd, rtp_channel, 96, 90000)
                        : (ports.has_value() && video_sender.open(client_ip, ports->first, 96, 90000));
                    if (!ok) {
                        send_all(client_fd, response_text("461 Unsupported Transport", cseq));
                        continue;
                    }
                    video_setup = true;
                    server_rtp = video_sender.local_port();
                }
                std::ostringstream transport;
                if (tcp_interleaved) {
                    transport << "RTP/AVP/TCP;unicast;interleaved=" << static_cast<int>(rtp_channel) << "-" << static_cast<int>(rtcp_channel);
                } else {
                    const auto server_rtcp = static_cast<std::uint16_t>(server_rtp + 1);
                    transport << "RTP/AVP/UDP;unicast;client_port=" << ports->first << "-" << ports->second
                              << ";server_port=" << server_rtp << "-" << server_rtcp;
                }
                send_all(client_fd, response_text("200 OK", cseq, {{"Transport", transport.str()}, {"Session", session_id}}));
                continue;
            }

            if (request.method == "PLAY") {
                if (!subscribed) {
                    const auto cached_messages = registry_.cached_messages(stream_key);
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        pending_messages.clear();
                        media_origin_timestamp_ms.reset();
                        playback_wall_origin.reset();
                        live_buffer_started_at.reset();
                        live_playback_started = false;
                        for (const auto& message : cached_messages) {
                            if (message.type_id == 8 || message.type_id == 9) {
                                pending_messages.push_back(message);
                            }
                        }
                    }
                    callback_id = next_callback_id_.fetch_add(1);
                    registry_.add_live_callback_subscriber(stream_key, callback_id, [&](const otts::rtmp::MediaMessage& message) {
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            pending_messages.push_back(message);
                        }
                        queue_cv.notify_one();
                    });
                    subscribed = true;
                    queue_cv.notify_all();
                    registry_.upsert_external_session(
                        session_key,
                        stream_key,
                        otts::media::StreamSource::Rtsp,
                        "play",
                        "cpp-rtsp-play",
                        "running",
                        request.uri,
                        "rtsp://0.0.0.0:" + std::to_string(port_),
                        audio_setup ? "stream-registry-h264-aac-rtp" : "stream-registry-h264-rtp",
                        (video_setup || audio_setup) ? "rtsp/rtp" : "rtsp/rtp/udp",
                        "cxx-rtsp-control-stream-registry-rtp",
                        "native-cxx",
                        audio_setup ? "h264/aac" : "h264 video",
                        0,
                        now_epoch_ms(),
                        0,
                        0,
                        0,
                        "");
                }
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}, {"RTP-Info", audio_setup ? "url=trackID=0;seq=0;rtptime=0,url=trackID=1;seq=0;rtptime=0" : "url=trackID=0;seq=0;rtptime=0"}}));
                continue;
            }

            if (request.method == "PAUSE") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request.method == "TEARDOWN") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                cleanup();
                return;
            }

            send_all(client_fd, response_text("405 Method Not Allowed", cseq));
        }
    }
}

}  // namespace otts::rtsp
