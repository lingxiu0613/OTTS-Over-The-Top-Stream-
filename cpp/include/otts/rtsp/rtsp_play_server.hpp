#pragma once

#include "otts/rtmp/stream_registry.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace otts::rtsp {

class RtspPlayServer {
public:
    RtspPlayServer(std::uint16_t port, otts::rtmp::StreamRegistry& registry);
    ~RtspPlayServer();

    bool start();
    void stop();

private:
    void accept_loop();
    void handle_client(int client_fd);

    std::uint16_t port_{0};
    otts::rtmp::StreamRegistry& registry_;
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_callback_id_{100000};
};

}  // namespace otts::rtsp
