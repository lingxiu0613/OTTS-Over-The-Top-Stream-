#include "otts/http/http_server.hpp"

#include "otts/core/logger.hpp"
#include "otts/http/flv_mux.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

namespace otts::http {

namespace {

std::string url_decode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            const auto decoded_char = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            decoded.push_back(decoded_char);
            i += 2;
        } else if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }

    return decoded;
}

std::string to_lower_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::vector<std::uint8_t> base64_decode(const std::string& input) {
    static const std::string kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> reverse_table(256, -1);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        reverse_table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int>(i);
    }

    std::vector<std::uint8_t> output;
    int value = 0;
    int bits = -8;
    for (const unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto decoded = reverse_table[ch];
        if (decoded < 0) {
            return {};
        }
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<std::uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

ssize_t send_without_sigpipe(int socket_fd, const void* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
    return ::send(socket_fd, data, size, MSG_NOSIGNAL);
#else
    return ::send(socket_fd, data, size, 0);
#endif
}

bool send_all(int socket_fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = send_without_sigpipe(socket_fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool has_expect_100_continue(const std::string& request) {
    const auto header_end = request.find("\r\n\r\n");
    const auto header_text = request.substr(0, header_end == std::string::npos ? request.size() : header_end);
    std::istringstream header_stream(header_text);
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        constexpr std::string_view prefix = "Expect:";
        if (line.rfind(prefix, 0) == 0) {
            auto value = line.substr(prefix.size());
            value = to_lower_copy(value);
            return value.find("100-continue") != std::string::npos;
        }
    }
    return false;
}

std::size_t parse_content_length(const std::string& request) {
    const auto header_end = request.find("\r\n\r\n");
    const auto header_text = request.substr(0, header_end == std::string::npos ? request.size() : header_end);
    std::istringstream header_stream(header_text);
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }
        if (to_lower_copy(line.substr(0, colon_pos)) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoul(line.substr(colon_pos + 1)));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

std::optional<std::string> extract_header_value(const std::string& request, const std::string& header_name) {
    const auto header_end = request.find("\r\n\r\n");
    const auto header_text = request.substr(0, header_end == std::string::npos ? request.size() : header_end);
    std::istringstream header_stream(header_text);
    std::string line;
    const auto lowered_name = to_lower_copy(header_name);
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }
        auto key = to_lower_copy(line.substr(0, colon_pos));
        if (key != lowered_name) {
            continue;
        }
        auto value = line.substr(colon_pos + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        return value;
    }
    return std::nullopt;
}

void log_webrtc_request_debug(
    const std::string& method,
    const std::string& path,
    const std::string& request,
    const std::string& body) {
    const auto content_type = extract_header_value(request, "Content-Type").value_or("-");
    const auto transfer_encoding = extract_header_value(request, "Transfer-Encoding").value_or("-");
    const auto expect = extract_header_value(request, "Expect").value_or("-");
    otts::core::log_info(
        "http_webrtc",
        "method=" + method +
            " path=" + path +
            " body_len=" + std::to_string(body.size()) +
            " content_type=" + content_type +
            " transfer_encoding=" + transfer_encoding +
            " expect=" + expect);
}

bool has_chunked_transfer_encoding(const std::string& request) {
    const auto header = extract_header_value(request, "Transfer-Encoding");
    if (!header.has_value()) {
        return false;
    }
    return to_lower_copy(*header).find("chunked") != std::string::npos;
}

bool has_complete_chunked_body(const std::string& request) {
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }
    const auto body = request.substr(header_end + 4);
    return body.find("\r\n0\r\n\r\n") != std::string::npos || body == "0\r\n\r\n";
}

std::string decode_chunked_body(std::string_view body) {
    std::string decoded;
    std::size_t cursor = 0;

    while (cursor < body.size()) {
        const auto line_end = body.find("\r\n", cursor);
        if (line_end == std::string_view::npos) {
            break;
        }

        const auto size_text = std::string(body.substr(cursor, line_end - cursor));
        std::size_t chunk_size = 0;
        try {
            chunk_size = static_cast<std::size_t>(std::stoul(size_text, nullptr, 16));
        } catch (...) {
            break;
        }

        cursor = line_end + 2;
        if (chunk_size == 0) {
            break;
        }
        if (cursor + chunk_size > body.size()) {
            break;
        }

        decoded.append(body.substr(cursor, chunk_size));
        cursor += chunk_size;
        if (cursor + 2 <= body.size() && body.substr(cursor, 2) == "\r\n") {
            cursor += 2;
        }
    }

    return decoded;
}

otts::media::StreamSource parse_stream_source(const std::string& value) {
    if (value == "whip") {
        return otts::media::StreamSource::Whip;
    }
    if (value == "rtsp") {
        return otts::media::StreamSource::Rtsp;
    }
    if (value == "srt") {
        return otts::media::StreamSource::Srt;
    }
    if (value == "rtmp") {
        return otts::media::StreamSource::Rtmp;
    }
    return otts::media::StreamSource::Unknown;
}

}  // namespace

HttpServer::HttpServer(
    std::uint16_t port,
    otts::rtmp::StreamRegistry& registry,
    otts::webrtc::WebRtcService& webrtc_service,
    std::function<std::string()> system_status_provider)
    : port_(port),
      registry_(registry),
      webrtc_service_(webrtc_service),
      system_status_provider_(std::move(system_status_provider)) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        otts::core::log_error("http_server", "failed to create socket");
        return false;
    }

    const auto flags = ::fcntl(listen_fd_, F_GETFD);
    if (flags >= 0) {
        ::fcntl(listen_fd_, F_SETFD, flags | FD_CLOEXEC);
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        otts::core::log_error("http_server", std::string("bind failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 64) < 0) {
        otts::core::log_error("http_server", std::string("listen failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    std::thread(&HttpServer::accept_loop, this).detach();
    otts::core::log_info("http_server", "listening on 0.0.0.0:" + std::to_string(port_));
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);

        const int client_fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (client_fd < 0) {
            if (running_.load()) {
                otts::core::log_warn("http_server", std::string("accept failed: ") + std::strerror(errno));
            }
            continue;
        }

        std::thread(&HttpServer::handle_client, this, client_fd).detach();
    }
}

void HttpServer::handle_client(int client_fd) {
    std::string request;
    request.reserve(16384);

    char buffer[8192];
    bool sent_continue = false;
    while (true) {
        const auto bytes = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            if (request.empty()) {
                ::close(client_fd);
                return;
            }
            break;
        }
        request.append(buffer, static_cast<std::size_t>(bytes));

        const auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }
        const auto content_length = parse_content_length(request);
        const auto chunked = has_chunked_transfer_encoding(request);
        const auto total_needed = header_end + 4 + content_length;
        if (!sent_continue &&
            ((chunked && !has_complete_chunked_body(request)) || (!chunked && request.size() < total_needed)) &&
            has_expect_100_continue(request)) {
            static constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
            if (!send_all(client_fd, kContinue)) {
                ::close(client_fd);
                return;
            }
            sent_continue = true;
        }
        if (chunked) {
            if (has_complete_chunked_body(request)) {
                break;
            }
            continue;
        }
        if (request.size() >= total_needed) {
            request.resize(total_needed);
            break;
        }
    }

    std::istringstream request_stream(request);
    std::string method;
    std::string path;
    std::string version;
    request_stream >> method >> path >> version;

    const auto normalized_path = extract_path_without_query(path);
    if (method == "GET" && normalized_path.size() > 4 &&
        normalized_path.rfind(".flv") == normalized_path.size() - 4) {
        handle_flv_request(client_fd, path);
        return;
    }

    const auto response = handle_request(request);
    send_without_sigpipe(client_fd, response.data(), response.size());
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

std::string HttpServer::handle_request(const std::string& request) {
    std::istringstream stream(request);
    std::string method;
    std::string path;
    std::string version;
    stream >> method >> path >> version;
    const auto forwarded_host = extract_header_value(request, "Host").value_or("127.0.0.1:8080");

    if (method == "GET" && path == "/api/health") {
        return make_json_response(
            "{\"ok\":true,\"service\":\"otts\",\"protocol\":\"rtmp\",\"dataPlane\":{\"protocol\":\"rtmp\",\"listen\":1935},\"http_api_port\":" +
            std::to_string(port_) + "}");
    }

    if (method == "GET" && path == "/api/streams") {
        return make_json_response(build_streams_json());
    }

    if (method == "GET" && path == "/api/sessions") {
        return make_json_response(build_protocol_sessions_json());
    }

    if (method == "GET" && path == "/api/maintenance") {
        return make_json_response(build_maintenance_json());
    }

    if (method == "POST" && path.rfind("/api/maintenance/cleanup", 0) == 0) {
        auto parse_u64 = [](const std::optional<std::string>& value, std::uint64_t fallback) {
            if (!value.has_value()) {
                return fallback;
            }
            try {
                return static_cast<std::uint64_t>(std::stoull(*value));
            } catch (...) {
                return fallback;
            }
        };
        const auto external_idle_ms = parse_u64(extract_query_param(path, "external_idle_ms"), 30000);
        const auto stopped_retention_ms = parse_u64(extract_query_param(path, "stopped_retention_ms"), 60000);
        const auto delta = registry_.cleanup_stale(external_idle_ms, stopped_retention_ms);
        std::ostringstream body;
        body << "{\"ok\":true,"
             << "\"expired_subscribers\":" << delta.expired_subscribers << ","
             << "\"inactive_external_publishers\":" << delta.inactive_external_publishers << ","
             << "\"removed_streams\":" << delta.removed_streams << ","
             << "\"removed_external_sessions\":" << delta.removed_external_sessions << ","
             << "\"last_run_epoch_ms\":" << delta.last_run_epoch_ms << "}";
        return make_json_response(body.str());
    }

    if (method == "GET" && path.rfind("/api/rtsp/describe", 0) == 0) {
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        if (stream_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        const auto describe = registry_.rtsp_describe_info(stream_key);
        if (!describe.has_value()) {
            return make_json_response("{\"ok\":false,\"error\":\"stream not ready\"}", "404 Not Found");
        }
        std::ostringstream body;
        body << "{";
        body << "\"ok\":true,";
        body << "\"stream_key\":\"" << describe->stream_key << "\",";
        body << "\"video_codec\":\"" << describe->video_codec << "\",";
        body << "\"profile_level_id\":\"" << describe->profile_level_id << "\",";
        body << "\"sprop_parameter_sets\":\"" << describe->sprop_parameter_sets << "\"";
        body << "}";
        return make_json_response(body.str());
    }

    if (method == "GET" && path == "/api/system/status") {
        if (system_status_provider_) {
            return make_json_response(system_status_provider_());
        }
        return make_json_response("{\"ok\":false,\"error\":\"system status unavailable\"}", "503 Service Unavailable");
    }

    if (method == "GET" && path == "/api/webrtc/sessions") {
        return make_json_response(build_webrtc_sessions_json());
    }

    if (method == "GET" && path == "/api/webrtc/native") {
        return make_json_response(build_webrtc_native_json());
    }

    if (method == "GET" && path == "/api/debug/flv") {
        return make_json_response(build_flv_stats_json());
    }

    if (method == "POST" && path.rfind("/api/internal/streams/upsert", 0) == 0) {
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        const auto source_protocol = extract_query_param(path, "source_protocol").value_or("unknown");
        const auto audio_codec = extract_query_param(path, "audio_codec").value_or("");
        const auto video_codec = extract_query_param(path, "video_codec").value_or("");
        const auto managed_by = extract_query_param(path, "managed_by").value_or("");
        const auto has_publisher = extract_query_param(path, "has_publisher").value_or("true") != "false";
        if (stream_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        registry_.upsert_external_stream(
            stream_key,
            parse_stream_source(source_protocol),
            audio_codec,
            video_codec,
            managed_by,
            has_publisher);
        return make_json_response("{\"ok\":true}");
    }

    if (method == "POST" && path.rfind("/api/internal/streams/remove", 0) == 0) {
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        const auto source_protocol = extract_query_param(path, "source_protocol").value_or("unknown");
        if (stream_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        registry_.remove_external_stream(stream_key, parse_stream_source(source_protocol));
        return make_json_response("{\"ok\":true}");
    }

    if (method == "POST" && path.rfind("/api/internal/streams/viewers", 0) == 0) {
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        const auto source_protocol = extract_query_param(path, "source_protocol").value_or("unknown");
        const auto managed_by = extract_query_param(path, "managed_by").value_or("");
        const auto viewer_count_text = extract_query_param(path, "viewer_count").value_or("0");
        if (stream_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        std::size_t viewer_count = 0;
        try {
            viewer_count = static_cast<std::size_t>(std::stoull(viewer_count_text));
        } catch (...) {
            viewer_count = 0;
        }
        registry_.update_external_viewers(
            stream_key,
            parse_stream_source(source_protocol),
            managed_by,
            viewer_count);
        return make_json_response("{\"ok\":true}");
    }

    if (method == "POST" && path.rfind("/api/internal/sessions/upsert", 0) == 0) {
        const auto session_key = extract_query_param(path, "session_key").value_or("");
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        const auto source_protocol = extract_query_param(path, "source_protocol").value_or("unknown");
        const auto direction = extract_query_param(path, "direction").value_or("");
        const auto managed_by = extract_query_param(path, "managed_by").value_or("");
        const auto state = extract_query_param(path, "state").value_or("");
        const auto public_url = extract_query_param(path, "public_url").value_or("");
        const auto bind_url = extract_query_param(path, "bind_url").value_or("");
        const auto target_url = extract_query_param(path, "target_url").value_or("");
        const auto transport = extract_query_param(path, "transport").value_or("");
        const auto media_path = extract_query_param(path, "media_path").value_or("");
        const auto native_stage = extract_query_param(path, "native_stage").value_or("");
        const auto codec_hint = extract_query_param(path, "codec_hint").value_or("");
        const auto pid_text = extract_query_param(path, "pid").value_or("0");
        const auto started_text = extract_query_param(path, "started_at_epoch_ms").value_or("0");
        const auto stopped_text = extract_query_param(path, "last_stopped_at_epoch_ms").value_or("0");
        const auto restart_text = extract_query_param(path, "restart_count").value_or("0");
        const auto exit_text = extract_query_param(path, "last_exit_code").value_or("0");
        const auto last_error = extract_query_param(path, "last_error").value_or("");
        if (session_key.empty() || stream_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing session_key or stream_key\"}", "400 Bad Request");
        }
        std::int64_t pid = 0;
        std::uint64_t started_at_epoch_ms = 0;
        std::uint64_t last_stopped_at_epoch_ms = 0;
        std::uint64_t restart_count = 0;
        std::int64_t last_exit_code = 0;
        try { pid = std::stoll(pid_text); } catch (...) {}
        try { started_at_epoch_ms = static_cast<std::uint64_t>(std::stoull(started_text)); } catch (...) {}
        try { last_stopped_at_epoch_ms = static_cast<std::uint64_t>(std::stoull(stopped_text)); } catch (...) {}
        try { restart_count = static_cast<std::uint64_t>(std::stoull(restart_text)); } catch (...) {}
        try { last_exit_code = std::stoll(exit_text); } catch (...) {}
        registry_.upsert_external_session(
            session_key,
            stream_key,
            parse_stream_source(source_protocol),
            direction,
            managed_by,
            state,
            public_url,
            bind_url,
            target_url,
            transport,
            media_path,
            native_stage,
            codec_hint,
            pid,
            started_at_epoch_ms,
            last_stopped_at_epoch_ms,
            restart_count,
            last_exit_code,
            last_error);
        return make_json_response("{\"ok\":true}");
    }

    if (method == "POST" && path.rfind("/api/internal/sessions/remove", 0) == 0) {
        const auto session_key = extract_query_param(path, "session_key").value_or("");
        if (session_key.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing session_key\"}", "400 Bad Request");
        }
        registry_.remove_external_session(session_key);
        return make_json_response("{\"ok\":true}");
    }

    if (method == "POST" && path.rfind("/api/internal/media/publish", 0) == 0) {
        const auto request_body = extract_request_body(request);
        const auto stream_key = extract_query_param(path, "stream_key").value_or("");
        const auto source_protocol = extract_query_param(path, "source_protocol").value_or("unknown");
        const auto managed_by = extract_query_param(path, "managed_by").value_or("");
        const auto message_type_text = extract_query_param(path, "message_type").value_or("0");
        const auto timestamp_text = extract_query_param(path, "timestamp").value_or("0");
        const auto message_stream_id_text = extract_query_param(path, "message_stream_id").value_or("1");
        auto payload_base64 = extract_query_param(path, "payload_base64").value_or("");
        if (payload_base64.empty()) {
            payload_base64 = request_body;
        }
        if (stream_key.empty() || payload_base64.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key or payload\"}", "400 Bad Request");
        }
        otts::rtmp::MediaMessage message;
        try {
            message.type_id = static_cast<std::uint8_t>(std::stoul(message_type_text));
            message.timestamp = static_cast<std::uint32_t>(std::stoul(timestamp_text));
            message.message_stream_id = static_cast<std::uint32_t>(std::stoul(message_stream_id_text));
        } catch (...) {
            return make_json_response("{\"ok\":false,\"error\":\"invalid media params\"}", "400 Bad Request");
        }
        message.payload = base64_decode(payload_base64);
        if (message.payload.empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"invalid payload_base64\"}", "400 Bad Request");
        }
        registry_.publish_external_media(
            stream_key,
            parse_stream_source(source_protocol),
            managed_by,
            message);
        return make_json_response("{\"ok\":true}");
    }

    const auto body = extract_request_body(request);
    if (path.rfind("/whip", 0) == 0 || path.rfind("/whep", 0) == 0 || path.rfind("/rtc/v1/", 0) == 0 ||
        path.rfind("/session/", 0) == 0 || path.rfind("/resource/", 0) == 0) {
        log_webrtc_request_debug(method, path, request, body);
    }
    if (method == "GET" &&
        (path.rfind("/whep/offer/v1", 0) == 0 || path.rfind("/rtc/v1/whep/offer", 0) == 0)) {
        const auto stream_key = extract_webrtc_stream_key(path);
        if (!stream_key || stream_key->empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        if (webrtc_service_.should_use_gateway()) {
            return make_json_response(
                "{\"ok\":false,\"error\":\"native WebRTC runtime is not selected\"}",
                "409 Conflict");
        }
        const auto result = webrtc_service_.create_native_play_offer(*stream_key);
        if (!result.ok) {
            return make_json_response(
                std::string("{\"ok\":false,\"error\":\"") + result.error + "\"}",
                "500 Internal Server Error");
        }
        return make_sdp_response(result.answer_sdp, result.session_id, forwarded_host);
    }

    if (method == "POST" &&
        (path.rfind("/whip", 0) == 0 || path.rfind("/rtc/v1/whip", 0) == 0)) {
        return handle_whip_request(path, body, forwarded_host);
    }

    if (method == "POST" &&
        (path.rfind("/whep", 0) == 0 || path.rfind("/rtc/v1/whep", 0) == 0)) {
        return handle_whep_request(path, body, forwarded_host);
    }

    if ((method == "PATCH" || method == "POST" || method == "DELETE") &&
        (path.rfind("/session/", 0) == 0 || path.rfind("/resource/", 0) == 0)) {
        return handle_webrtc_session_request(method, path, body, forwarded_host);
    }

    if (method == "POST" && path.rfind("/api/streams/disconnect", 0) == 0) {
        const bool disconnected = handle_disconnect_request(path);
        return make_json_response(std::string("{\"ok\":") + (disconnected ? "true" : "false") + "}");
    }

    return make_text_response("not found", "404 Not Found");
}

void HttpServer::handle_flv_request(int client_fd, const std::string& request_path) {
    struct FlvClientState {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::vector<std::uint8_t>> queue;
        bool active{true};
        bool slow_disconnect{false};
        std::size_t queued_bytes{0};
    };

    const auto stream_key = extract_stream_key_from_flv_path(request_path);
    if (stream_key.empty()) {
        const auto response = make_text_response("bad flv path", "400 Bad Request");
        send_without_sigpipe(client_fd, response.data(), response.size());
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    const auto header = make_flv_file_header(true, true);
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: video/x-flv\r\n";
    response << "Connection: close\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "Access-Control-Allow-Origin: *\r\n\r\n";
    const auto prelude = response.str();
    if (send_without_sigpipe(client_fd, prelude.data(), prelude.size()) <= 0 ||
        send_without_sigpipe(client_fd, reinterpret_cast<const char*>(header.data()), header.size()) <= 0) {
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    on_flv_client_connected(stream_key);

    constexpr std::size_t kMaxQueuedTags = 256;
    constexpr std::size_t kMaxQueuedBytes = 8 * 1024 * 1024;
    auto client_state = std::make_shared<FlvClientState>();
    const auto callback_id = next_callback_id_.fetch_add(1);

    registry_.add_callback_subscriber(
        stream_key,
        callback_id,
        [client_state](const otts::rtmp::MediaMessage& message) {
            std::lock_guard<std::mutex> lock(client_state->mutex);
            if (!client_state->active) {
                return;
            }

            const auto tag = make_flv_tag(message);
            if (client_state->queue.size() >= kMaxQueuedTags ||
                client_state->queued_bytes + tag.size() > kMaxQueuedBytes) {
                client_state->slow_disconnect = true;
                client_state->active = false;
                client_state->condition.notify_all();
                return;
            }

            client_state->queued_bytes += tag.size();
            client_state->queue.push_back(tag);
            client_state->condition.notify_one();
        });

    while (true) {
        std::vector<std::uint8_t> tag;
        {
            std::unique_lock<std::mutex> lock(client_state->mutex);
            client_state->condition.wait_for(
                lock,
                std::chrono::milliseconds(200),
                [&] { return !client_state->active || !client_state->queue.empty(); });

            if (!client_state->queue.empty()) {
                tag = std::move(client_state->queue.front());
                client_state->queued_bytes -= tag.size();
                client_state->queue.pop_front();
            } else if (!client_state->active) {
                break;
            }
        }

        if (!tag.empty()) {
            const auto bytes = send_without_sigpipe(client_fd, reinterpret_cast<const char*>(tag.data()), tag.size());
            if (bytes <= 0) {
                std::lock_guard<std::mutex> lock(client_state->mutex);
                client_state->active = false;
                break;
            }
            on_flv_bytes_sent(stream_key, static_cast<std::size_t>(bytes));
        } else {
            char probe = 0;
            const auto bytes = ::recv(client_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (bytes == 0) {
                std::lock_guard<std::mutex> lock(client_state->mutex);
                client_state->active = false;
                break;
            }
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::lock_guard<std::mutex> lock(client_state->mutex);
                client_state->active = false;
                break;
            }
        }
    }

    registry_.remove_callback_subscriber(stream_key, callback_id);
    {
        std::lock_guard<std::mutex> lock(client_state->mutex);
        client_state->active = false;
        client_state->condition.notify_all();
    }
    on_flv_client_disconnected(stream_key, client_state->slow_disconnect);
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

std::string HttpServer::make_json_response(const std::string& body, const std::string& status) const {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    return response.str();
}

std::string HttpServer::make_text_response(const std::string& body, const std::string& status) const {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    return response.str();
}

std::string HttpServer::make_sdp_response(
    const std::string& body,
    const std::string& session_id,
    const std::string& forwarded_host,
    const std::string& status) const {
    const auto host = forwarded_host.empty() ? std::string("127.0.0.1:") + std::to_string(port_) : forwarded_host;
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: application/sdp\r\n";
    response << "Location: /session/" << session_id << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Expose-Headers: Location\r\n";
    response << "X-OTTS-Session-URL: http://" << host << "/session/" << session_id << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;
    return response.str();
}

std::string HttpServer::build_streams_json() const {
    const auto streams = registry_.snapshots();
    std::ostringstream body;
    body << "{\"streams\":[";

    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto& stream = streams[i];
        if (i > 0) {
            body << ",";
        }

        body << "{";
        body << "\"stream_key\":\"" << stream.stream_key << "\",";
        body << "\"source_protocol\":\"" << stream.source_protocol << "\",";
        body << "\"ingest_origin\":\"" << stream.ingest_origin << "\",";
        body << "\"managed_by\":\"" << stream.managed_by << "\",";
        body << "\"has_publisher\":" << (stream.has_publisher ? "true" : "false") << ",";
        body << "\"viewer_count\":" << stream.viewer_count << ",";
        body << "\"http_flv_viewer_count\":" << stream.callback_viewer_count << ",";
        body << "\"external_viewer_count\":" << stream.external_viewer_count << ",";
        body << "\"total_viewer_count\":" << (stream.viewer_count + stream.callback_viewer_count + stream.external_viewer_count) << ",";
        body << "\"has_metadata\":" << (stream.has_metadata ? "true" : "false") << ",";
        body << "\"has_audio_sequence_header\":" << (stream.has_audio_sequence_header ? "true" : "false") << ",";
        body << "\"has_video_sequence_header\":" << (stream.has_video_sequence_header ? "true" : "false") << ",";
        body << "\"has_keyframe\":" << (stream.has_keyframe ? "true" : "false") << ",";
        body << "\"ready_for_play\":" << (stream.ready_for_play ? "true" : "false") << ",";
        body << "\"audio_codec\":\"" << stream.audio_codec << "\",";
        body << "\"video_codec\":\"" << stream.video_codec << "\",";
        body << "\"track_count\":" << stream.track_count << ",";
        body << "\"gop_cache_size\":" << stream.gop_cache_size << ",";
        body << "\"total_packets\":" << stream.total_packets << ",";
        body << "\"total_bytes\":" << stream.total_bytes << ",";
        body << "\"average_packet_rate\":" << std::fixed << std::setprecision(2) << stream.average_packet_rate << ",";
        body << "\"average_bitrate_kbps\":" << std::fixed << std::setprecision(2) << stream.average_bitrate_kbps << ",";
        body << "\"audio_packets\":" << stream.audio_packets << ",";
        body << "\"audio_bytes\":" << stream.audio_bytes << ",";
        body << "\"video_packets\":" << stream.video_packets << ",";
        body << "\"video_bytes\":" << stream.video_bytes << ",";
        body << "\"data_packets\":" << stream.data_packets << ",";
        body << "\"data_bytes\":" << stream.data_bytes << ",";
        body << "\"last_media_timestamp\":" << stream.last_media_timestamp << ",";
        body << "\"last_keyframe_at_epoch_ms\":" << stream.last_keyframe_at_epoch_ms << ",";
        body << "\"first_media_at_epoch_ms\":" << stream.first_media_at_epoch_ms << ",";
        body << "\"last_media_at_epoch_ms\":" << stream.last_media_at_epoch_ms << ",";
        body << "\"last_media_age_ms\":" << stream.last_media_age_ms;
        {
            std::lock_guard<std::mutex> stats_lock(flv_stats_mutex_);
            const auto stats_it = flv_stats_.find(stream.stream_key);
            if (stats_it != flv_stats_.end()) {
                body << ",\"http_flv_connections_active\":" << stats_it->second.active_connections;
                body << ",\"http_flv_connections_total\":" << stats_it->second.total_connections;
                body << ",\"http_flv_slow_disconnects\":" << stats_it->second.slow_disconnects;
                body << ",\"http_flv_bytes_sent\":" << stats_it->second.bytes_sent;
            } else {
                body << ",\"http_flv_connections_active\":0";
                body << ",\"http_flv_connections_total\":0";
                body << ",\"http_flv_slow_disconnects\":0";
                body << ",\"http_flv_bytes_sent\":0";
            }
        }
        body << "}";
    }

    body << "]}";
    return body.str();
}

std::string HttpServer::build_protocol_sessions_json() const {
    const auto sessions = registry_.external_sessions();
    std::ostringstream body;
    body << "{\"sessions\":[";

    for (std::size_t i = 0; i < sessions.size(); ++i) {
        const auto& session = sessions[i];
        if (i > 0) {
            body << ",";
        }
        body << "{";
        body << "\"session_key\":\"" << session.session_key << "\",";
        body << "\"stream_key\":\"" << session.stream_key << "\",";
        body << "\"source_protocol\":\"" << session.source_protocol << "\",";
        body << "\"direction\":\"" << session.direction << "\",";
        body << "\"managed_by\":\"" << session.managed_by << "\",";
        body << "\"state\":\"" << session.state << "\",";
        body << "\"public_url\":\"" << session.public_url << "\",";
        body << "\"bind_url\":\"" << session.bind_url << "\",";
        body << "\"target_url\":\"" << session.target_url << "\",";
        body << "\"pid\":" << session.pid << ",";
        body << "\"started_at_epoch_ms\":" << session.started_at_epoch_ms << ",";
        body << "\"updated_at_epoch_ms\":" << session.updated_at_epoch_ms << ",";
        body << "\"last_stopped_at_epoch_ms\":" << session.last_stopped_at_epoch_ms << ",";
        body << "\"restart_count\":" << session.restart_count << ",";
        body << "\"last_exit_code\":" << session.last_exit_code << ",";
        body << "\"last_error\":\"" << session.last_error << "\"";
        body << "}";
    }

    body << "]}";
    return body.str();
}

std::string HttpServer::build_maintenance_json() const {
    const auto stats = registry_.cleanup_stats();
    std::ostringstream body;
    body << "{";
    body << "\"ok\":true,";
    body << "\"cleanup\":{";
    body << "\"runs\":" << stats.runs << ",";
    body << "\"expired_subscribers\":" << stats.expired_subscribers << ",";
    body << "\"inactive_external_publishers\":" << stats.inactive_external_publishers << ",";
    body << "\"removed_streams\":" << stats.removed_streams << ",";
    body << "\"removed_external_sessions\":" << stats.removed_external_sessions << ",";
    body << "\"last_run_epoch_ms\":" << stats.last_run_epoch_ms;
    body << "}";
    body << "}";
    return body.str();
}

std::string HttpServer::build_flv_stats_json() const {
    std::lock_guard<std::mutex> lock(flv_stats_mutex_);
    std::ostringstream body;
    body << "{\"streams\":[";

    bool first = true;
    for (const auto& [stream_key, stats] : flv_stats_) {
        if (!first) {
            body << ",";
        }
        first = false;
        body << "{";
        body << "\"stream_key\":\"" << stream_key << "\",";
        body << "\"active_connections\":" << stats.active_connections << ",";
        body << "\"total_connections\":" << stats.total_connections << ",";
        body << "\"slow_disconnects\":" << stats.slow_disconnects << ",";
        body << "\"bytes_sent\":" << stats.bytes_sent;
        body << "}";
    }

    body << "]}";
    return body.str();
}

std::string HttpServer::build_webrtc_sessions_json() const {
    const auto sessions = webrtc_service_.snapshots();
    std::ostringstream body;
    body << "{\"sessions\":[";

    for (std::size_t i = 0; i < sessions.size(); ++i) {
        const auto& session = sessions[i];
        if (i > 0) {
            body << ",";
        }
        body << "{";
        body << "\"session_id\":\"" << session.session_id << "\",";
        body << "\"stream_key\":\"" << session.stream_key << "\",";
        body << "\"direction\":\"" << session.direction << "\",";
        body << "\"state\":\"" << session.state << "\",";
        body << "\"offer_size\":" << session.offer_size << ",";
        body << "\"answer_size\":" << session.answer_size << ",";
        body << "\"created_at_epoch_ms\":" << session.created_at_epoch_ms << ",";
        body << "\"updated_at_epoch_ms\":" << session.updated_at_epoch_ms << ",";
        body << "\"transport_state\":\"" << session.transport_state << "\",";
        body << "\"video_frames\":" << session.video_frames << ",";
        body << "\"video_bytes\":" << session.video_bytes << ",";
        body << "\"audio_frames\":" << session.audio_frames << ",";
        body << "\"audio_bytes\":" << session.audio_bytes << ",";
        body << "\"last_error\":\"" << session.last_error << "\"";
        body << "}";
    }

    body << "]}";
    return body.str();
}

std::string HttpServer::build_webrtc_native_json() const {
    const auto status = webrtc_service_.native_status();
    auto mode_to_string = [](otts::webrtc::RuntimeMode mode) {
        switch (mode) {
            case otts::webrtc::RuntimeMode::Gateway:
                return "gateway";
            case otts::webrtc::RuntimeMode::Auto:
                return "auto";
            case otts::webrtc::RuntimeMode::Native:
                return "native";
        }
        return "gateway";
    };

    std::ostringstream body;
    body << "{";
    body << "\"ok\":true,";
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

bool HttpServer::handle_disconnect_request(const std::string& request_path) {
    const auto query_pos = request_path.find('?');
    if (query_pos == std::string::npos) {
        return false;
    }

    const auto query = request_path.substr(query_pos + 1);
    const auto key_pos = query.find("stream_key=");
    if (key_pos == std::string::npos) {
        return false;
    }

    auto value = query.substr(key_pos + std::strlen("stream_key="));
    const auto ampersand_pos = value.find('&');
    if (ampersand_pos != std::string::npos) {
        value = value.substr(0, ampersand_pos);
    }

    return registry_.disconnect_stream(url_decode(value));
}

std::optional<std::string> HttpServer::extract_query_param(
    const std::string& request_path,
    const std::string& key) const {
    const auto query_pos = request_path.find('?');
    if (query_pos == std::string::npos) {
        return std::nullopt;
    }
    const auto query = request_path.substr(query_pos + 1);
    const auto key_pos = query.find(key + "=");
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }

    auto value = query.substr(key_pos + key.size() + 1);
    const auto ampersand_pos = value.find('&');
    if (ampersand_pos != std::string::npos) {
        value = value.substr(0, ampersand_pos);
    }
    return url_decode(value);
}

std::string HttpServer::extract_request_body(const std::string& request) const {
    const auto separator = request.find("\r\n\r\n");
    if (separator == std::string::npos) {
        return {};
    }
    const auto body = request.substr(separator + 4);
    if (has_chunked_transfer_encoding(request)) {
        return decode_chunked_body(body);
    }
    return body;
}

std::string HttpServer::handle_whip_request(
    const std::string& request_path,
    const std::string& request_body,
    const std::string& forwarded_host) {
    if (!webrtc_service_.should_use_gateway()) {
        const auto stream_key = extract_webrtc_stream_key(request_path);
        if (!stream_key || stream_key->empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        const auto result = webrtc_service_.handle_native_offer(
            otts::webrtc::SessionDirection::Publish,
            *stream_key,
            request_body);
        if (!result.ok) {
            return make_json_response(
                std::string("{\"ok\":false,\"error\":\"") + result.error + "\"}",
                "500 Internal Server Error");
        }
        return make_sdp_response(result.answer_sdp, result.session_id, forwarded_host);
    }
    if (request_path.rfind("/rtc/v1/whip", 0) == 0) {
        auto mapped = std::string("/whip/v1");
        const auto query_pos = request_path.find('?');
        if (query_pos != std::string::npos) {
            mapped += request_path.substr(query_pos);
        }
        return proxy_webrtc_request("POST", mapped, request_body, "application/sdp", forwarded_host);
    }
    return proxy_webrtc_request("POST", request_path, request_body, "application/sdp", forwarded_host);
}

std::string HttpServer::handle_whep_request(
    const std::string& request_path,
    const std::string& request_body,
    const std::string& forwarded_host) {
    if (!webrtc_service_.should_use_gateway()) {
        const auto stream_key = extract_webrtc_stream_key(request_path);
        if (!stream_key || stream_key->empty()) {
            return make_json_response("{\"ok\":false,\"error\":\"missing stream_key\"}", "400 Bad Request");
        }
        const auto result = webrtc_service_.handle_native_offer(
            otts::webrtc::SessionDirection::Play,
            *stream_key,
            request_body);
        if (!result.ok) {
            return make_json_response(
                std::string("{\"ok\":false,\"error\":\"") + result.error + "\"}",
                "500 Internal Server Error");
        }
        return make_sdp_response(result.answer_sdp, result.session_id, forwarded_host);
    }
    if (request_path.rfind("/rtc/v1/whep", 0) == 0) {
        auto mapped = std::string("/whep/v1");
        const auto query_pos = request_path.find('?');
        if (query_pos != std::string::npos) {
            mapped += request_path.substr(query_pos);
        }
        return proxy_webrtc_request("POST", mapped, request_body, "application/sdp", forwarded_host);
    }
    return proxy_webrtc_request("POST", request_path, request_body, "application/sdp", forwarded_host);
}

std::optional<std::string> HttpServer::extract_webrtc_stream_key(const std::string& request_path) const {
    if (auto stream_key = extract_query_param(request_path, "stream_key")) {
        return stream_key;
    }
    const auto app = extract_query_param(request_path, "app");
    const auto stream = extract_query_param(request_path, "stream");
    if (app && stream && !app->empty() && !stream->empty()) {
        return *app + "/" + *stream;
    }
    return std::nullopt;
}

std::string HttpServer::handle_webrtc_session_request(
    const std::string& method,
    const std::string& request_path,
    const std::string& request_body,
    const std::string& forwarded_host) {
    if (!webrtc_service_.should_use_gateway()) {
        if (method == "POST" && request_path.rfind("/session/", 0) == 0) {
            auto session_path = request_path.substr(std::strlen("/session/"));
            const auto query_pos = session_path.find('?');
            if (query_pos != std::string::npos) {
                session_path = session_path.substr(0, query_pos);
            }
            const auto answer_suffix = std::string("/answer");
            if (session_path.size() > answer_suffix.size() &&
                session_path.compare(session_path.size() - answer_suffix.size(), answer_suffix.size(), answer_suffix) == 0) {
                auto session_id = session_path.substr(0, session_path.size() - answer_suffix.size());
                const auto accepted = webrtc_service_.set_native_answer(url_decode(session_id), request_body);
                return make_json_response(std::string("{\"ok\":") + (accepted ? "true" : "false") + "}");
            }
        }
        if (method == "DELETE" && request_path.rfind("/session/", 0) == 0) {
            auto session_id = request_path.substr(std::strlen("/session/"));
            const auto query_pos = session_id.find('?');
            if (query_pos != std::string::npos) {
                session_id = session_id.substr(0, query_pos);
            }
            const auto closed = webrtc_service_.close_session(url_decode(session_id));
            return make_json_response(std::string("{\"ok\":") + (closed ? "true" : "false") + "}");
        }
        if (method == "PATCH") {
            return make_json_response("{\"ok\":true,\"trickle_ice\":\"ignored\"}");
        }
        return make_json_response("{\"ok\":false,\"error\":\"unsupported native WebRTC session method\"}", "405 Method Not Allowed");
    }
    return proxy_webrtc_request(
        method,
        request_path,
        request_body,
        "application/trickle-ice-sdpfrag",
        forwarded_host);
}

std::string HttpServer::proxy_webrtc_request(
    const std::string& method,
    const std::string& target_path,
    const std::string& request_body,
    const std::string& content_type,
    const std::string& forwarded_host) {
    const int proxy_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) {
        return make_json_response("{\"ok\":false,\"error\":\"failed to create proxy socket\"}", "502 Bad Gateway");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8081);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(proxy_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(proxy_fd);
        return make_json_response("{\"ok\":false,\"error\":\"webrtc gateway unavailable\"}", "502 Bad Gateway");
    }

    std::ostringstream request_stream;
    request_stream << method << " " << target_path << " HTTP/1.1\r\n";
    request_stream << "Host: 127.0.0.1:8081\r\n";
    if (!forwarded_host.empty()) {
        request_stream << "X-Forwarded-Host: " << forwarded_host << "\r\n";
    }
    request_stream << "Content-Type: " << content_type << "\r\n";
    request_stream << "Content-Length: " << request_body.size() << "\r\n";
    request_stream << "Connection: close\r\n\r\n";
    request_stream << request_body;

    const auto outbound = request_stream.str();
    if (!send_all(proxy_fd, outbound)) {
        ::close(proxy_fd);
        return make_json_response("{\"ok\":false,\"error\":\"failed to send request to gateway\"}", "502 Bad Gateway");
    }

    std::string upstream_response;
    upstream_response.reserve(8192);
    char buffer[8192];
    std::optional<std::size_t> expected_total_size;

    while (true) {
        const auto bytes = ::recv(proxy_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            break;
        }
        upstream_response.append(buffer, static_cast<std::size_t>(bytes));

        if (!expected_total_size.has_value()) {
            const auto header_end = upstream_response.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::size_t content_length = 0;
                std::istringstream header_stream(upstream_response.substr(0, header_end));
                std::string line;
                while (std::getline(header_stream, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    constexpr std::string_view prefix = "Content-Length:";
                    if (line.rfind(prefix, 0) == 0) {
                        try {
                            content_length = static_cast<std::size_t>(std::stoul(line.substr(prefix.size())));
                        } catch (...) {
                            content_length = 0;
                        }
                        break;
                    }
                }
                expected_total_size = header_end + 4 + content_length;
            }
        }

        if (expected_total_size.has_value() && upstream_response.size() >= *expected_total_size) {
            upstream_response.resize(*expected_total_size);
            break;
        }
    }
    ::close(proxy_fd);

    if (upstream_response.empty()) {
        return make_json_response("{\"ok\":false,\"error\":\"empty gateway response\"}", "502 Bad Gateway");
    }

    if (!forwarded_host.empty()) {
        const auto location_pos = upstream_response.find("\r\nLocation: /session/");
        if (location_pos != std::string::npos) {
            const auto value_start = location_pos + std::strlen("\r\nLocation: ");
            const auto value_end = upstream_response.find("\r\n", value_start);
            if (value_end != std::string::npos) {
                const auto relative = upstream_response.substr(value_start, value_end - value_start);
                const auto absolute = std::string("http://") + forwarded_host + relative;
                upstream_response.replace(value_start, value_end - value_start, absolute);
            }
        }
    }

    return upstream_response;
}

std::string HttpServer::extract_path_without_query(const std::string& request_path) {
    const auto query_pos = request_path.find('?');
    if (query_pos == std::string::npos) {
        return request_path;
    }
    return request_path.substr(0, query_pos);
}

std::string HttpServer::extract_stream_key_from_flv_path(const std::string& request_path) {
    auto path = extract_path_without_query(request_path);
    if (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.size() < 5 || path.rfind(".flv") != path.size() - 4) {
        return {};
    }
    return path.substr(0, path.size() - 4);
}

void HttpServer::on_flv_client_connected(const std::string& stream_key) {
    std::lock_guard<std::mutex> lock(flv_stats_mutex_);
    auto& stats = flv_stats_[stream_key];
    stats.active_connections += 1;
    stats.total_connections += 1;
}

void HttpServer::on_flv_client_disconnected(const std::string& stream_key, bool slow_disconnect) {
    std::lock_guard<std::mutex> lock(flv_stats_mutex_);
    auto& stats = flv_stats_[stream_key];
    if (stats.active_connections > 0) {
        stats.active_connections -= 1;
    }
    if (slow_disconnect) {
        stats.slow_disconnects += 1;
    }
}

void HttpServer::on_flv_bytes_sent(const std::string& stream_key, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(flv_stats_mutex_);
    flv_stats_[stream_key].bytes_sent += static_cast<std::uint64_t>(bytes);
}

}  // namespace otts::http
