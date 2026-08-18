#include "otts/rtsp/rtsp_publish_server.hpp"

#include "otts/auth/stream_auth.hpp"
#include "otts/core/logger.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace otts::rtsp {
namespace {

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

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
    std::string body;
};

std::optional<RtspRequest> parse_request(const std::string& raw) {
    const auto body_pos = raw.find("\r\n\r\n");
    const auto head = raw.substr(0, body_pos == std::string::npos ? raw.size() : body_pos);
    RtspRequest request;
    if (body_pos != std::string::npos) {
        request.body = raw.substr(body_pos + 4);
    }
    std::istringstream stream(head);
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
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return request;
}

std::size_t content_length_from_head(const std::string& head) {
    std::istringstream stream(head);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (lower(trim(line.substr(0, colon))) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoul(trim(line.substr(colon + 1))));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

std::string response_text(
    const std::string& status,
    const std::string& cseq,
    const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::ostringstream response;
    response << "RTSP/1.0 " << status << "\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: OTTS-CXX-RTSP-PUBLISH/0.1\r\n";
    for (const auto& [key, value] : headers) {
        response << key << ": " << value << "\r\n";
    }
    response << "\r\n";
    return response.str();
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_client_ports(const std::string& transport) {
    const auto key = lower(transport).find("client_port=");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const auto start = key + std::string("client_port=").size();
    const auto dash = transport.find('-', start);
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::make_pair(
            static_cast<std::uint16_t>(std::stoul(transport.substr(start, dash - start))),
            static_cast<std::uint16_t>(std::stoul(transport.substr(dash + 1))));
    } catch (...) {
        return std::nullopt;
    }
}

int bind_udp_port(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len) == 0) {
        port = ntohs(address.sin_port);
    }
    return fd;
}

struct NalSets {
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

std::vector<std::uint8_t> base64_decode(const std::string& input) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = -8;
    for (const unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto decoded = table[ch];
        if (decoded < 0) {
            return {};
        }
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

NalSets parse_sdp_sets(const std::string& sdp) {
    NalSets sets;
    std::istringstream lines(sdp);
    std::string line;
    while (std::getline(lines, line)) {
        const auto marker = std::string("sprop-parameter-sets=");
        const auto pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        auto value = line.substr(pos + marker.size());
        const auto semicolon = value.find(';');
        if (semicolon != std::string::npos) {
            value = value.substr(0, semicolon);
        }
        const auto comma = value.find(',');
        if (comma != std::string::npos) {
            sets.sps = base64_decode(value.substr(0, comma));
            sets.pps = base64_decode(value.substr(comma + 1));
        }
    }
    return sets;
}

void extract_sets_from_annexb(const std::vector<std::uint8_t>& au, NalSets& sets) {
    for (std::size_t i = 0; i + 4 < au.size();) {
        std::size_t code = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            code = 3;
        } else if (i + 4 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            code = 4;
        } else {
            ++i;
            continue;
        }
        const auto start = i + code;
        auto end = start;
        while (end + 3 < au.size() && !(au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 || (end + 3 < au.size() && au[end + 2] == 0 && au[end + 3] == 1)))) {
            ++end;
        }
        if (end > start) {
            const auto type = au[start] & 0x1f;
            if (type == 7) {
                sets.sps.assign(au.begin() + static_cast<std::ptrdiff_t>(start), au.begin() + static_cast<std::ptrdiff_t>(end));
            } else if (type == 8) {
                sets.pps.assign(au.begin() + static_cast<std::ptrdiff_t>(start), au.begin() + static_cast<std::ptrdiff_t>(end));
            }
        }
        i = end;
    }
}

bool has_idr(const std::vector<std::uint8_t>& au) {
    for (std::size_t i = 0; i + 4 < au.size(); ++i) {
        const bool code3 = au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1;
        const bool code4 = i + 4 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1;
        if (code3 || code4) {
            const auto nal = i + (code3 ? 3 : 4);
            if (nal < au.size() && (au[nal] & 0x1f) == 5) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::uint8_t> annexb_to_avcc(const std::vector<std::uint8_t>& au) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 3 < au.size();) {
        std::size_t code = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            code = 3;
        } else if (i + 4 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            code = 4;
        } else {
            ++i;
            continue;
        }
        const auto start = i + code;
        auto end = start;
        while (end + 3 < au.size() && !(au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 || (end + 3 < au.size() && au[end + 2] == 0 && au[end + 3] == 1)))) {
            ++end;
        }
        while (end < au.size() && au[end] == 0) {
            ++end;
        }
        const auto size = end > start ? end - start : 0;
        if (size > 0) {
            write_be32(out, static_cast<std::uint32_t>(size));
            out.insert(out.end(), au.begin() + static_cast<std::ptrdiff_t>(start), au.begin() + static_cast<std::ptrdiff_t>(end));
        }
        i = end;
    }
    return out;
}

otts::rtmp::MediaMessage make_avc_sequence_header(std::uint32_t timestamp, const NalSets& sets) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload = {0x17, 0x00, 0x00, 0x00, 0x00, 0x01};
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

otts::rtmp::MediaMessage make_avc_nalu_message(std::uint32_t timestamp, const std::vector<std::uint8_t>& au) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload.push_back(static_cast<std::uint8_t>((has_idr(au) ? 0x10 : 0x20) | 0x07));
    message.payload.insert(message.payload.end(), {0x01, 0x00, 0x00, 0x00});
    auto avcc = annexb_to_avcc(au);
    message.payload.insert(message.payload.end(), avcc.begin(), avcc.end());
    return message;
}

class H264RtpIngest {
public:
    H264RtpIngest(otts::rtmp::StreamRegistry& registry, std::string stream_key, NalSets sets)
        : registry_(registry), stream_key_(std::move(stream_key)), sets_(std::move(sets)) {}

    void handle_packet(const std::uint8_t* packet, std::size_t size) {
        if (size < 12 || (packet[0] >> 6) != 2) {
            return;
        }
        const bool marker = (packet[1] & 0x80) != 0;
        const auto csrc_count = packet[0] & 0x0f;
        std::size_t offset = 12 + static_cast<std::size_t>(csrc_count) * 4;
        if (offset >= size) {
            return;
        }
        if ((packet[0] & 0x10) != 0) {
            if (offset + 4 > size) {
                return;
            }
            const auto ext_len = static_cast<std::size_t>((packet[offset + 2] << 8) | packet[offset + 3]) * 4;
            offset += 4 + ext_len;
            if (offset >= size) {
                return;
            }
        }
        const auto rtp_ts =
            (static_cast<std::uint32_t>(packet[4]) << 24) |
            (static_cast<std::uint32_t>(packet[5]) << 16) |
            (static_cast<std::uint32_t>(packet[6]) << 8) |
            static_cast<std::uint32_t>(packet[7]);
        if (!base_ts_.has_value()) {
            base_ts_ = rtp_ts;
        }
        const auto timestamp_ms = static_cast<std::uint32_t>((static_cast<std::uint64_t>(rtp_ts - *base_ts_) * 1000) / 90000);
        const auto* payload = packet + offset;
        const auto payload_size = size - offset;
        if (payload_size == 0) {
            return;
        }

        const auto nal_type = payload[0] & 0x1f;
        if (nal_type >= 1 && nal_type <= 23) {
            append_start_code();
            current_au_.insert(current_au_.end(), payload, payload + payload_size);
        } else if (nal_type == 24) {
            std::size_t cursor = 1;
            while (cursor + 2 <= payload_size) {
                const auto nal_size = static_cast<std::size_t>((payload[cursor] << 8) | payload[cursor + 1]);
                cursor += 2;
                if (nal_size == 0 || cursor + nal_size > payload_size) {
                    break;
                }
                append_start_code();
                current_au_.insert(current_au_.end(), payload + cursor, payload + cursor + nal_size);
                cursor += nal_size;
            }
        } else if (nal_type == 28 && payload_size >= 2) {
            const bool start = (payload[1] & 0x80) != 0;
            const auto reconstructed = static_cast<std::uint8_t>((payload[0] & 0xe0) | (payload[1] & 0x1f));
            if (start) {
                append_start_code();
                current_au_.push_back(reconstructed);
            }
            current_au_.insert(current_au_.end(), payload + 2, payload + payload_size);
        }

        if (marker && !current_au_.empty()) {
            publish(timestamp_ms);
            current_au_.clear();
        }
    }

private:
    void append_start_code() {
        current_au_.insert(current_au_.end(), {0x00, 0x00, 0x00, 0x01});
    }

    void publish(std::uint32_t timestamp_ms) {
        extract_sets_from_annexb(current_au_, sets_);
        if (!sent_sequence_ && !sets_.sps.empty() && !sets_.pps.empty()) {
            registry_.publish_external_media(
                stream_key_,
                otts::media::StreamSource::Rtsp,
                "cpp-rtsp-native-publish",
                make_avc_sequence_header(timestamp_ms, sets_));
            sent_sequence_ = true;
        }
        auto message = make_avc_nalu_message(timestamp_ms, current_au_);
        if (message.payload.size() > 5) {
            registry_.publish_external_media(stream_key_, otts::media::StreamSource::Rtsp, "cpp-rtsp-native-publish", message);
        }
    }

    otts::rtmp::StreamRegistry& registry_;
    std::string stream_key_;
    NalSets sets_;
    std::optional<std::uint32_t> base_ts_;
    std::vector<std::uint8_t> current_au_;
    bool sent_sequence_{false};
};

}  // namespace

RtspPublishServer::RtspPublishServer(std::uint16_t port, otts::rtmp::StreamRegistry& registry)
    : port_(port), registry_(registry) {}

RtspPublishServer::~RtspPublishServer() {
    stop();
}

bool RtspPublishServer::start() {
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
        otts::core::log_error("rtsp_publish", std::string("bind failed: ") + std::strerror(errno));
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
    std::thread(&RtspPublishServer::accept_loop, this).detach();
    otts::core::log_info("rtsp_publish", "C++ RTSP publish listening on 0.0.0.0:" + std::to_string(port_));
    return true;
}

void RtspPublishServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void RtspPublishServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0) {
            continue;
        }
        std::thread(&RtspPublishServer::handle_client, this, fd).detach();
    }
}

void RtspPublishServer::handle_client(int client_fd) {
    const auto session_id = std::to_string(next_session_id_.fetch_add(1));
    const auto session_key = "cpp-rtsp-publish:" + session_id;
    std::string buffer;
    std::string stream_key;
    NalSets sets;
    std::uint16_t rtp_port = 0;
    std::uint16_t rtcp_port = 0;
    int rtp_fd = -1;
    int rtcp_fd = -1;
    std::atomic<bool> receiving{false};
    std::thread receiver;

    auto cleanup = [&]() {
        receiving.store(false);
        if (rtp_fd >= 0) {
            ::shutdown(rtp_fd, SHUT_RDWR);
            ::close(rtp_fd);
            rtp_fd = -1;
        }
        if (rtcp_fd >= 0) {
            ::shutdown(rtcp_fd, SHUT_RDWR);
            ::close(rtcp_fd);
            rtcp_fd = -1;
        }
        if (receiver.joinable()) {
            receiver.join();
        }
        if (!stream_key.empty()) {
            registry_.remove_external_session(session_key);
            registry_.remove_external_stream(stream_key, otts::media::StreamSource::Rtsp);
        }
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
            const auto head = buffer.substr(0, end + 4);
            const auto content_length = content_length_from_head(head);
            if (buffer.size() < end + 4 + content_length) {
                break;
            }
            const auto raw = buffer.substr(0, end + 4 + content_length);
            buffer.erase(0, end + 4 + content_length);
            const auto request = parse_request(raw);
            if (!request.has_value()) {
                cleanup();
                return;
            }
            const auto cseq_it = request->headers.find("cseq");
            const auto cseq = cseq_it == request->headers.end() ? "1" : cseq_it->second;

            if (request->method == "OPTIONS") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Public", "OPTIONS, ANNOUNCE, SETUP, RECORD, GET_PARAMETER, SET_PARAMETER, TEARDOWN"}}));
                continue;
            }
            if (stream_key.empty()) {
                stream_key = stream_key_from_uri(request->uri);
            }
            if (!otts::auth::is_authorized(
                    "publish",
                    stream_key,
                    query_value(request->uri, "token"),
                    query_value(request->uri, "expires"),
                    query_value(request->uri, "sign"))) {
                send_all(client_fd, response_text("401 Unauthorized", cseq));
                cleanup();
                return;
            }
            if (request->method == "ANNOUNCE") {
                sets = parse_sdp_sets(request->body);
                registry_.upsert_external_stream(stream_key, otts::media::StreamSource::Rtsp, "", "h264", "cpp-rtsp-native-publish", true);
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "announced",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    "h264",
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                send_all(client_fd, response_text("200 OK", cseq));
                continue;
            }
            if (request->method == "SETUP") {
                const auto transport = request->headers.find("transport");
                if (transport == request->headers.end() || !parse_client_ports(transport->second).has_value()) {
                    send_all(client_fd, response_text("461 Unsupported Transport", cseq));
                    continue;
                }
                for (int attempt = 0; attempt < 32; ++attempt) {
                    rtp_port = 0;
                    rtp_fd = bind_udp_port(rtp_port);
                    if (rtp_fd < 0 || (rtp_port % 2) != 0) {
                        if (rtp_fd >= 0) {
                            ::close(rtp_fd);
                            rtp_fd = -1;
                        }
                        continue;
                    }
                    rtcp_port = static_cast<std::uint16_t>(rtp_port + 1);
                    rtcp_fd = bind_udp_port(rtcp_port);
                    if (rtcp_fd >= 0) {
                        break;
                    }
                    ::close(rtp_fd);
                    rtp_fd = -1;
                }
                if (rtp_fd < 0 || rtcp_fd < 0) {
                    send_all(client_fd, response_text("500 Internal Server Error", cseq));
                    continue;
                }
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "setup",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    "h264",
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                std::ostringstream transport_response;
                const auto ports = parse_client_ports(transport->second).value();
                transport_response << "RTP/AVP/UDP;unicast;client_port=" << ports.first << "-" << ports.second
                                   << ";server_port=" << rtp_port << "-" << rtcp_port;
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}, {"Transport", transport_response.str()}}));
                continue;
            }
            if (request->method == "RECORD") {
                if (rtp_fd < 0) {
                    send_all(client_fd, response_text("455 Method Not Valid in This State", cseq));
                    continue;
                }
                receiving.store(true);
                receiver = std::thread([&, local_rtp_fd = rtp_fd]() {
                    H264RtpIngest ingest(registry_, stream_key, sets);
                    std::array<std::uint8_t, 2048> packet{};
                    while (receiving.load()) {
                        const auto received = ::recv(local_rtp_fd, packet.data(), packet.size(), 0);
                        if (received <= 0) {
                            break;
                        }
                        ingest.handle_packet(packet.data(), static_cast<std::size_t>(received));
                    }
                });
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "recording",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    "h264",
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request->method == "GET_PARAMETER" || request->method == "SET_PARAMETER") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request->method == "TEARDOWN") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                cleanup();
                return;
            }
            send_all(client_fd, response_text("405 Method Not Allowed", cseq));
        }
    }
}

}  // namespace otts::rtsp
