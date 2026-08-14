#pragma once

#include "otts/rtmp/stream_registry.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace otts::rtmp {

class RtmpServer {
public:
    explicit RtmpServer(std::uint16_t port);
    ~RtmpServer();

    bool start();
    void stop();
    StreamRegistry& registry();
    const StreamRegistry& registry() const;

private:
    void accept_loop();

    std::uint16_t port_;
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    StreamRegistry registry_;
};

}  // namespace otts::rtmp
