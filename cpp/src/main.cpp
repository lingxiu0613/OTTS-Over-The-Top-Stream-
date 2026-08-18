#include "otts/core/logger.hpp"
#include "otts/core/process_supervisor.hpp"
#include "otts/http/http_server.hpp"
#include "otts/rtmp/rtmp_server.hpp"
#include "otts/rtsp/rtsp_play_server.hpp"
#include "otts/rtsp/rtsp_publish_server.hpp"
#include "otts/srt/srt_native_server.hpp"
#include "otts/webrtc/webrtc_service.hpp"

#include <atomic>
#include <cstdlib>
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

std::string env_string(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value == nullptr || std::string(value).empty() ? std::move(fallback) : std::string(value);
}

std::uint16_t env_port(const char* name, std::uint16_t fallback) {
    const auto value = env_string(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        const auto parsed = std::stoul(value);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<std::uint16_t>(parsed);
        }
    } catch (...) {
    }
    return fallback;
}

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const auto value = env_string(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

otts::webrtc::RuntimeMode parse_webrtc_runtime_mode(const std::string& value) {
    if (value == "native") {
        return otts::webrtc::RuntimeMode::Native;
    }
    if (value == "gateway") {
        return otts::webrtc::RuntimeMode::Gateway;
    }
    return otts::webrtc::RuntimeMode::Auto;
}

otts::webrtc::NativeStatus build_webrtc_native_status(const std::string& mode_text, const std::string& dependency_root) {
    otts::webrtc::NativeStatus status;
    status.configured_mode = parse_webrtc_runtime_mode(mode_text);
    status.compiled_with_dependency = (OTTS_WEBRTC_NATIVE_DEPENDENCY != 0) || (OTTS_WEBRTC_DATACHANNEL != 0);
    status.dependency_root = dependency_root;
    status.dependency_ready = status.compiled_with_dependency && !dependency_root.empty() && std::filesystem::exists(dependency_root);
    status.peer_factory_ready = false;
    status.media_engine_ready = false;
#if OTTS_WEBRTC_DATACHANNEL
    status.dependency_ready = true;
    status.peer_factory_ready = true;
    status.media_engine_ready = true;
    status.selected_runtime =
        (status.configured_mode == otts::webrtc::RuntimeMode::Gateway) ? "gateway" : "native";
    status.detail = "native WHIP/WHEP H.264 + Opus path enabled through libdatachannel";
#else
    status.selected_runtime =
        (status.configured_mode == otts::webrtc::RuntimeMode::Native) ? "native" : "gateway";
    if (!status.compiled_with_dependency) {
        status.detail = "compiled without libwebrtc native dependency";
    } else if (!status.dependency_ready) {
        status.detail = "compiled with native option but dependency root is not available";
    } else {
        status.detail = "libwebrtc dependency detected; PeerConnection media engine wiring remains";
    }
#endif
    return status;
}

std::vector<std::string> child_environment(
    const std::vector<std::pair<std::string, std::string>>& defaults) {
    std::vector<std::string> env;
    for (const auto& [key, fallback] : defaults) {
        env.push_back(key + "=" + env_string(key.c_str(), fallback));
    }
    return env;
}

}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    const auto rtmp_port = env_port("OTTS_RTMP_PORT", 1935);
    const auto http_api_port = env_port("OTTS_HTTP_API_PORT", 8080);
    const auto compat_http_port = env_port("OTTS_COMPAT_HTTP_PORT", 1985);
    const auto node_http_port = env_port("PORT", 3000);
    const auto node_https_port = env_port("HTTPS_PORT", 3443);
    const auto webrtc_gateway_port = env_port("OTTS_WEBRTC_GATEWAY_PORT", 8081);
    const auto cpp_rtsp_publish_port = env_port("OTTS_CPP_RTSP_PUBLISH_PORT", env_port("OTTS_RTSP_PUBLISH_PORT", 0));
    const auto cpp_rtsp_play_port = env_port("OTTS_CPP_RTSP_PLAY_PORT", 0);
    const auto cpp_srt_publish_port = env_port("OTTS_CPP_SRT_PUBLISH_PORT", env_port("OTTS_SRT_PUBLISH_PORT_BASE", 0));
    const auto cpp_srt_play_port = env_port("OTTS_CPP_SRT_PLAY_PORT", env_port("OTTS_SRT_PLAY_PORT_BASE", 0));
    const auto cpp_srt_stream_key = env_string("OTTS_CPP_SRT_STREAM_KEY", "live/srt-demo");
    const auto cleanup_interval_ms = env_u64("OTTS_CLEANUP_INTERVAL_MS", 5000);
    const auto external_publisher_idle_ms = env_u64("OTTS_EXTERNAL_PUBLISHER_IDLE_MS", 30000);
    const auto stopped_session_retention_ms = env_u64("OTTS_STOPPED_SESSION_RETENTION_MS", 60000);

    otts::rtmp::RtmpServer server(rtmp_port);
    if (!server.start()) {
        otts::core::log_error("main", "failed to start RTMP server");
        return 1;
    }

    otts::rtsp::RtspPublishServer cpp_rtsp_publish(cpp_rtsp_publish_port, server.registry());
    if (!cpp_rtsp_publish.start()) {
        otts::core::log_error("main", "failed to start C++ RTSP publish server");
        return 1;
    }

    otts::rtsp::RtspPlayServer cpp_rtsp_play(cpp_rtsp_play_port, server.registry());
    if (!cpp_rtsp_play.start()) {
        otts::core::log_error("main", "failed to start C++ RTSP play server");
        return 1;
    }

    otts::srt::SrtNativeServer cpp_srt(cpp_srt_publish_port, cpp_srt_play_port, cpp_srt_stream_key, server.registry());
    if (!cpp_srt.start()) {
        otts::core::log_error("main", "failed to start C++ SRT native server");
        return 1;
    }

    const auto project_root = detect_project_root();
    const auto python_dir = (project_root / "python").string();
    const auto node_dir = (project_root / "node").string();
    const auto cert_key = (project_root / "node" / "certs" / "otts.key").string();
    const auto cert_crt = (project_root / "node" / "certs" / "otts.crt").string();
    const auto stream_token = env_string("OTTS_STREAM_TOKEN");
    const auto auth_secret = env_string("OTTS_AUTH_SECRET");
    const auto auth_ttl = env_string("OTTS_AUTH_TTL_SECONDS", "3600");
    const auto webrtc_mode = env_string("OTTS_WEBRTC_MODE", "auto");
    const auto libwebrtc_root = env_string("OTTS_LIBWEBRTC_ROOT");
    const auto libdatachannel_root = env_string("OTTS_LIBDATACHANNEL_ROOT");
#if OTTS_WEBRTC_DATACHANNEL
    const auto webrtc_dependency_root = libdatachannel_root.empty() ? libwebrtc_root : libdatachannel_root;
#else
    const auto webrtc_dependency_root = libwebrtc_root.empty() ? libdatachannel_root : libwebrtc_root;
#endif
    otts::webrtc::WebRtcService webrtc_service(build_webrtc_native_status(webrtc_mode, webrtc_dependency_root));
    webrtc_service.attach_registry(server.registry());

    otts::core::ManagedProcess webrtc_gateway;
    const auto should_start_webrtc_gateway = webrtc_service.should_use_gateway();
    if (should_start_webrtc_gateway) {
        webrtc_gateway.start(
            "webrtc_gateway",
            python_dir,
            {
                "python3",
                "-u",
                "webrtc_gateway.py",
                "--host",
                "0.0.0.0",
                "--port",
                std::to_string(webrtc_gateway_port),
                "--core-rtmp-base",
                "rtmp://127.0.0.1:" + std::to_string(rtmp_port),
                "--core-http-flv-base",
                "http://127.0.0.1:" + std::to_string(http_api_port),
                "--enable-rtmp-bridge",
            },
            child_environment({
                {"OTTS_STREAM_TOKEN", stream_token},
                {"OTTS_AUTH_SECRET", auth_secret},
                {"OTTS_AUTH_TTL_SECONDS", auth_ttl},
            }),
            "/tmp/otts_webrtc.out",
            "/tmp/otts_webrtc.err");
    } else {
        otts::core::log_warn("main", "OTTS_WEBRTC_MODE=native selected; Python WebRTC gateway will not be started");
    }

    otts::core::ManagedProcess node_console;
    node_console.start(
        "node_console",
        node_dir,
        {"node", "src/server.js"},
        child_environment({
            {"PORT", std::to_string(node_http_port)},
            {"HTTPS_PORT", std::to_string(node_https_port)},
            {"OTTS_API_BASE", "http://127.0.0.1:" + std::to_string(http_api_port)},
            {"OTTS_WEBRTC_GATEWAY_BASE", "http://127.0.0.1:" + std::to_string(webrtc_gateway_port)},
            {"OTTS_TLS_KEY_PATH", cert_key},
            {"OTTS_TLS_CERT_PATH", cert_crt},
            {"OTTS_STREAM_TOKEN", stream_token},
            {"OTTS_AUTH_SECRET", auth_secret},
            {"OTTS_AUTH_TTL_SECONDS", auth_ttl},
            {"OTTS_RTMP_BASE", "rtmp://127.0.0.1:" + std::to_string(rtmp_port)},
            {"OTTS_RTSP_PUBLIC_HOST", env_string("OTTS_RTSP_PUBLIC_HOST", env_string("OTTS_PUBLIC_HOST", "192.168.40.11"))},
            {"OTTS_SRT_PUBLIC_HOST", env_string("OTTS_SRT_PUBLIC_HOST", env_string("OTTS_PUBLIC_HOST", "192.168.40.11"))},
            {"OTTS_RTSP_PUBLISH_PORT", env_string("OTTS_RTSP_PUBLISH_PORT", "8554")},
            {"OTTS_RTSP_PLAY_PORT", env_string("OTTS_RTSP_PLAY_PORT", "8556")},
            {"OTTS_SRT_PUBLISH_PORT_BASE", env_string("OTTS_SRT_PUBLISH_PORT_BASE", "9000")},
            {"OTTS_SRT_PLAY_PORT_BASE", env_string("OTTS_SRT_PLAY_PORT_BASE", "10000")},
            {"OTTS_RTSP_PUBLISH_MODE", env_string("OTTS_RTSP_PUBLISH_MODE", "core-direct-flv")},
            {"OTTS_RTSP_PLAY_MODE", env_string("OTTS_RTSP_PLAY_MODE", "core-egress-flv")},
            {"OTTS_SRT_PUBLISH_MODE", env_string("OTTS_SRT_PUBLISH_MODE", "core-direct-flv")},
            {"OTTS_SRT_PLAY_MODE", env_string("OTTS_SRT_PLAY_MODE", "core-egress-flv")},
            {"OTTS_SRT_BOOTSTRAP_ENABLED", env_string("OTTS_SRT_BOOTSTRAP_ENABLED", "false")},
            {"OTTS_NATIVE_PROTOCOL_ONLY", env_string("OTTS_NATIVE_PROTOCOL_ONLY", "true")},
            {"OTTS_FFMPEG_BIN", env_string("OTTS_FFMPEG_BIN", "ffmpeg")},
            {"OTTS_RECORDING_ROOT", env_string("OTTS_RECORDING_ROOT", "/tmp/otts_recordings")},
        }),
        "/tmp/otts_node.out",
        "/tmp/otts_node.err");

    const auto started_at_epoch_ms = now_epoch_ms();
    auto system_status_provider = [
                                      &webrtc_gateway,
                                      &node_console,
                                      started_at_epoch_ms,
                                      rtmp_port,
                                      http_api_port,
                                      compat_http_port,
                                      cpp_rtsp_publish_port,
                                      cpp_rtsp_play_port,
                                      cpp_srt_publish_port,
                                      cpp_srt_play_port,
                                      cleanup_interval_ms,
                                      external_publisher_idle_ms,
                                      stopped_session_retention_ms,
                                      should_start_webrtc_gateway,
                                      &webrtc_service]() {
        const auto now_ms = now_epoch_ms();
        std::ostringstream body;
        body << "{";
        body << "\"ok\":true,";
        body << "\"service\":\"otts-core\",";
        body << "\"pid\":" << static_cast<int>(::getpid()) << ",";
        body << "\"started_at_epoch_ms\":" << started_at_epoch_ms << ",";
        body << "\"now_epoch_ms\":" << now_ms << ",";
        body << "\"uptime_ms\":" << (now_ms >= started_at_epoch_ms ? (now_ms - started_at_epoch_ms) : 0) << ",";
        body << "\"ports\":{"
             << "\"rtmp\":" << rtmp_port << ","
             << "\"http_api\":" << http_api_port << ","
             << "\"compat_http\":" << compat_http_port << ","
             << "\"cpp_rtsp_publish\":" << cpp_rtsp_publish_port << ","
             << "\"cpp_rtsp_play\":" << cpp_rtsp_play_port << ","
             << "\"cpp_srt_publish\":" << cpp_srt_publish_port << ","
             << "\"cpp_srt_play\":" << cpp_srt_play_port
             << "},";
        body << "\"maintenance\":{"
             << "\"cleanup_interval_ms\":" << cleanup_interval_ms << ","
             << "\"external_publisher_idle_ms\":" << external_publisher_idle_ms << ","
             << "\"stopped_session_retention_ms\":" << stopped_session_retention_ms
             << "},";
        body << "\"managed_processes\":[";
        if (should_start_webrtc_gateway) {
            body << build_process_json(webrtc_gateway.snapshot()) << ",";
        }
        body << build_process_json(node_console.snapshot());
        body << "],";
        const auto webrtc_status = webrtc_service.native_status();
        body << "\"webrtc_native\":{";
        body << "\"selected_runtime\":\"" << json_escape(webrtc_status.selected_runtime) << "\",";
        body << "\"compiled_with_dependency\":" << (webrtc_status.compiled_with_dependency ? "true" : "false") << ",";
        body << "\"dependency_ready\":" << (webrtc_status.dependency_ready ? "true" : "false") << ",";
        body << "\"peer_factory_ready\":" << (webrtc_status.peer_factory_ready ? "true" : "false") << ",";
        body << "\"media_engine_ready\":" << (webrtc_status.media_engine_ready ? "true" : "false") << ",";
        body << "\"detail\":\"" << json_escape(webrtc_status.detail) << "\"";
        body << "}}";
        return body.str();
    };

    otts::http::HttpServer http_server(http_api_port, server.registry(), webrtc_service, system_status_provider);
    if (!http_server.start()) {
        otts::core::log_error("main", "failed to start HTTP API server");
        node_console.stop();
        webrtc_gateway.stop();
        server.stop();
        return 1;
    }

    otts::http::HttpServer compat_http_server(compat_http_port, server.registry(), webrtc_service, system_status_provider);
    if (!compat_http_server.start()) {
        otts::core::log_error("main", "failed to start WHIP/WHEP compatibility server");
        http_server.stop();
        node_console.stop();
        webrtc_gateway.stop();
        server.stop();
        return 1;
    }

    otts::core::log_info("main", "OTTS native protocol core started with RTMP/FLV/HLS, RTSP, SRT, WHIP/WHEP services");

    auto last_cleanup_at = now_epoch_ms();
    while (g_running.load()) {
        const auto now_ms = now_epoch_ms();
        if (cleanup_interval_ms > 0 && now_ms >= last_cleanup_at &&
            now_ms - last_cleanup_at >= cleanup_interval_ms) {
            server.registry().cleanup_stale(external_publisher_idle_ms, stopped_session_retention_ms);
            webrtc_service.cleanup_stale_sessions(stopped_session_retention_ms);
            last_cleanup_at = now_ms;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    compat_http_server.stop();
    http_server.stop();
    cpp_srt.stop();
    cpp_rtsp_play.stop();
    cpp_rtsp_publish.stop();
    node_console.stop();
    webrtc_gateway.stop();
    server.stop();
    otts::core::log_info("main", "server stopped");
    return 0;
}
