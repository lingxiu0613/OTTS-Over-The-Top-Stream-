#include "otts/core/logger.hpp"
#include "otts/core/process_supervisor.hpp"
#include "otts/http/http_server.hpp"
#include "otts/rtmp/rtmp_server.hpp"
#include "otts/webrtc/webrtc_service.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running.store(false);
}

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (const auto ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string build_process_json(const otts::core::ManagedProcessSnapshot& snapshot) {
    std::ostringstream body;
    body << "{";
    body << "\"name\":\"" << json_escape(snapshot.name) << "\",";
    body << "\"pid\":" << snapshot.pid << ",";
    body << "\"running\":" << (snapshot.running ? "true" : "false") << ",";
    body << "\"started_at_epoch_ms\":" << snapshot.started_at_epoch_ms << ",";
    body << "\"workdir\":\"" << json_escape(snapshot.workdir) << "\",";
    body << "\"stdout_path\":\"" << json_escape(snapshot.stdout_path) << "\",";
    body << "\"stderr_path\":\"" << json_escape(snapshot.stderr_path) << "\"";
    body << "}";
    return body.str();
}

std::filesystem::path detect_project_root() {
#ifdef __linux__
    std::error_code ec;
    auto exe_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return exe_path.parent_path().parent_path();
    }
#endif
    return std::filesystem::current_path();
}

}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    otts::rtmp::RtmpServer server(1935);
    if (!server.start()) {
        otts::core::log_error("main", "failed to start RTMP server");
        return 1;
    }

    const auto project_root = detect_project_root();
    const auto python_dir = (project_root / "python").string();
    const auto node_dir = (project_root / "node").string();
    const auto cert_key = (project_root / "node" / "certs" / "otts.key").string();
    const auto cert_crt = (project_root / "node" / "certs" / "otts.crt").string();

    otts::core::ManagedProcess webrtc_gateway;
    webrtc_gateway.start(
        "webrtc_gateway",
        python_dir,
        {"python3", "-u", "webrtc_gateway.py", "--host", "0.0.0.0", "--port", "8081"},
        {},
        "/tmp/otts_webrtc.out",
        "/tmp/otts_webrtc.err");

    otts::core::ManagedProcess node_console;
    node_console.start(
        "node_console",
        node_dir,
        {"node", "src/server.js"},
        {
            "PORT=3000",
            "HTTPS_PORT=3443",
            "OTTS_API_BASE=http://127.0.0.1:8080",
            "OTTS_WEBRTC_GATEWAY_BASE=http://127.0.0.1:8081",
            "OTTS_TLS_KEY_PATH=" + cert_key,
            "OTTS_TLS_CERT_PATH=" + cert_crt,
        },
        "/tmp/otts_node.out",
        "/tmp/otts_node.err");

    const auto started_at_epoch_ms = now_epoch_ms();
    auto system_status_provider = [&webrtc_gateway, &node_console, started_at_epoch_ms]() {
        const auto now_ms = now_epoch_ms();
        std::ostringstream body;
        body << "{";
        body << "\"ok\":true,";
        body << "\"service\":\"otts-core\",";
        body << "\"pid\":" << static_cast<int>(::getpid()) << ",";
        body << "\"started_at_epoch_ms\":" << started_at_epoch_ms << ",";
        body << "\"now_epoch_ms\":" << now_ms << ",";
        body << "\"uptime_ms\":" << (now_ms >= started_at_epoch_ms ? (now_ms - started_at_epoch_ms) : 0) << ",";
        body << "\"ports\":{\"rtmp\":1935,\"http_api\":8080,\"compat_http\":1985},";
        body << "\"managed_processes\":[";
        body << build_process_json(webrtc_gateway.snapshot()) << ",";
        body << build_process_json(node_console.snapshot());
        body << "]}";
        return body.str();
    };

    otts::webrtc::WebRtcService webrtc_service;
    otts::http::HttpServer http_server(8080, server.registry(), webrtc_service, system_status_provider);
    if (!http_server.start()) {
        otts::core::log_error("main", "failed to start HTTP API server");
        node_console.stop();
        webrtc_gateway.stop();
        server.stop();
        return 1;
    }

    otts::http::HttpServer compat_http_server(1985, server.registry(), webrtc_service, system_status_provider);
    if (!compat_http_server.start()) {
        otts::core::log_error("main", "failed to start WHIP/WHEP compatibility server");
        http_server.stop();
        node_console.stop();
        webrtc_gateway.stop();
        server.stop();
        return 1;
    }

    otts::core::log_info("main", "OTTS phase-1 RTMP server started with managed WebRTC/Node services");

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    compat_http_server.stop();
    http_server.stop();
    node_console.stop();
    webrtc_gateway.stop();
    server.stop();
    otts::core::log_info("main", "server stopped");
    return 0;
}
