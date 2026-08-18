#pragma once

#include "otts/rtmp/stream_registry.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace otts::srt {

class SrtNativeServer {
public:
    SrtNativeServer(
        std::uint16_t publish_port,
        std::uint16_t play_port,
        std::string publish_stream_key,
        otts::rtmp::StreamRegistry& registry);
    ~SrtNativeServer();

    bool start();
    void stop();

private:
    void publish_loop();
    void play_loop();
    void handle_publish_client(int socket);
    void handle_play_client(int socket);

    std::uint16_t publish_port_{0};
    std::uint16_t play_port_{0};
    std::string publish_stream_key_;
    otts::rtmp::StreamRegistry& registry_;
    std::atomic<bool> running_{false};
    std::thread publish_thread_;
    std::thread play_thread_;
    int publish_socket_{-1};
    int play_socket_{-1};
};

}  // namespace otts::srt
