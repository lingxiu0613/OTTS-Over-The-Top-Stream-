#pragma once

#include "otts/rtmp/stream_registry.hpp"
#include "otts/webrtc/webrtc_service.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace otts::http {

class HttpServer {
public:
    HttpServer(
        std::uint16_t port,
        otts::rtmp::StreamRegistry& registry,
        otts::webrtc::WebRtcService& webrtc_service,
        std::function<std::string()> system_status_provider = {});
    ~HttpServer();

    bool start();
    void stop();

private:
    void accept_loop();
    void handle_client(int client_fd);
    std::string handle_request(const std::string& request);
    void handle_flv_request(int client_fd, const std::string& request_path);
    std::string make_json_response(const std::string& body, const std::string& status = "200 OK") const;
    std::string make_text_response(const std::string& body, const std::string& status) const;
    std::string make_sdp_response(
        const std::string& body,
        const std::string& session_id,
        const std::string& forwarded_host,
        const std::string& status = "201 Created") const;
    std::string build_streams_json() const;
    std::string build_protocol_sessions_json() const;
    std::string build_maintenance_json() const;
    std::string build_metrics_text() const;
    bool handle_disconnect_request(const std::string& request_path);
    std::string build_flv_stats_json() const;
    std::string build_webrtc_sessions_json() const;
    std::string build_webrtc_native_json() const;
    std::optional<std::string> extract_query_param(const std::string& request_path, const std::string& key) const;
    std::string extract_request_body(const std::string& request) const;
    std::string handle_whip_request(
        const std::string& request_path,
        const std::string& request_body,
        const std::string& forwarded_host);
    std::string handle_whep_request(
        const std::string& request_path,
        const std::string& request_body,
        const std::string& forwarded_host);
    std::optional<std::string> extract_webrtc_stream_key(const std::string& request_path) const;
    std::string handle_webrtc_session_request(
        const std::string& method,
        const std::string& request_path,
        const std::string& request_body,
        const std::string& forwarded_host);
    std::string proxy_webrtc_request(
        const std::string& method,
        const std::string& target_path,
        const std::string& request_body,
        const std::string& content_type = "application/sdp",
        const std::string& forwarded_host = "");
    static std::string extract_path_without_query(const std::string& request_path);
    static std::string extract_stream_key_from_flv_path(const std::string& request_path);

    struct FlvStreamStats {
        std::uint64_t active_connections{0};
        std::uint64_t total_connections{0};
        std::uint64_t slow_disconnects{0};
        std::uint64_t bytes_sent{0};
    };

    void on_flv_client_connected(const std::string& stream_key);
    void on_flv_client_disconnected(const std::string& stream_key, bool slow_disconnect);
    void on_flv_bytes_sent(const std::string& stream_key, std::size_t bytes);

    std::uint16_t port_;
    otts::rtmp::StreamRegistry& registry_;
    otts::webrtc::WebRtcService& webrtc_service_;
    std::function<std::string()> system_status_provider_;
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_callback_id_{1};
    mutable std::mutex flv_stats_mutex_;
    std::unordered_map<std::string, FlvStreamStats> flv_stats_;
};

}  // namespace otts::http
