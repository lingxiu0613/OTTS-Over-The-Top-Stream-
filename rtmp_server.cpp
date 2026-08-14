#include "otts/rtmp/rtmp_server.hpp"

#include "otts/core/logger.hpp"
#include "otts/rtmp/rtmp_session.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

namespace otts::rtmp {

RtmpServer::RtmpServer(std::uint16_t port) : port_(port) {}

RtmpServer::~RtmpServer() {
    stop();
}

bool RtmpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        otts::core::log_error("rtmp_server", "failed to create socket");
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        otts::core::log_error("rtmp_server", std::string("bind failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 64) < 0) {
        otts::core::log_error("rtmp_server", std::string("listen failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    std::thread(&RtmpServer::accept_loop, this).detach();
    otts::core::log_info("rtmp_server", "listening on 0.0.0.0:" + std::to_string(port_));
    return true;
}

void RtmpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void RtmpServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);

        const int client_fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (client_fd < 0) {
            if (running_.load()) {
                otts::core::log_warn("rtmp_server", std::string("accept failed: ") + std::strerror(errno));
            }
            continue;
        }

        const std::string client_ip = inet_ntoa(client_address.sin_addr);
        otts::core::log_info("rtmp_server", "accepted connection from " + client_ip);

        auto session = std::make_shared<RtmpSession>(client_fd, client_ip, registry_);
        session->start();
    }
}

StreamRegistry& RtmpServer::registry() {
    return registry_;
}

const StreamRegistry& RtmpServer::registry() const {
    return registry_;
}

}  // namespace otts::rtmp
