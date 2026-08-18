#pragma once

#include "otts/rtmp/amf0.hpp"
#include "otts/rtmp/stream_registry.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace otts::rtmp {

class RtmpSession : public std::enable_shared_from_this<RtmpSession> {
public:
    RtmpSession(int socket_fd, std::string client_ip, StreamRegistry& registry);
    ~RtmpSession();

    void start();
    void stop();
    void send_media(const MediaMessage& message);

private:
    struct ChunkState {
        std::uint32_t timestamp{0};
        std::uint32_t timestamp_delta{0};
        std::uint32_t message_length{0};
        std::uint8_t type_id{0};
        std::uint32_t message_stream_id{0};
        std::vector<std::uint8_t> payload;
        bool header_ready{false};
    };

    bool perform_handshake();
    void session_loop();
    bool receive_message(MediaMessage& message);
    void handle_message(const MediaMessage& message);
    void handle_command(const MediaMessage& message);
    void handle_control_message(const MediaMessage& message);
    void handle_media_message(const MediaMessage& message);

    void on_connect(const std::vector<Amf0Value>& values);
    void on_release_stream(const std::vector<Amf0Value>& values);
    void on_fc_publish(const std::vector<Amf0Value>& values);
    void on_fc_unpublish(const std::vector<Amf0Value>& values);
    void on_create_stream(const std::vector<Amf0Value>& values);
    void on_publish(const MediaMessage& message, const std::vector<Amf0Value>& values);
    void on_play(const MediaMessage& message, const std::vector<Amf0Value>& values);
    void on_delete_stream(const std::vector<Amf0Value>& values);

    bool read_exact(std::uint8_t* buffer, std::size_t size);
    bool write_all(const std::uint8_t* data, std::size_t size);
    void send_window_ack_size();
    void send_set_peer_bandwidth();
    void send_set_chunk_size(std::uint32_t chunk_size);
    void send_stream_begin(std::uint32_t stream_id);
    void send_on_status(std::uint32_t stream_id, const std::string& code, const std::string& description);
    bool is_stream_authorized(const std::string& action, const std::string& stream_key, const std::string& supplied_token, const std::string& expires, const std::string& signature) const;
    void reject_stream(std::uint32_t stream_id, const std::string& code, const std::string& description);
    void send_command_result(double transaction_id, const Amf0Object& properties, const Amf0Object& info);
    void send_simple_result(double transaction_id, const Amf0Value& value);
    void send_chunked_message(std::uint32_t chunk_stream_id, const MediaMessage& message);

    static std::uint32_t read_be24(const std::uint8_t* data);
    static std::uint32_t read_le32(const std::uint8_t* data);
    static void write_be24(std::vector<std::uint8_t>& out, std::uint32_t value);
    static void write_le32(std::vector<std::uint8_t>& out, std::uint32_t value);

    int socket_fd_;
    std::string client_ip_;
    StreamRegistry& registry_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;

    std::uint32_t inbound_chunk_size_{128};
    std::uint32_t outbound_chunk_size_{4096};
    std::uint32_t next_stream_id_{1};

    std::unordered_map<std::uint32_t, ChunkState> chunk_states_;

    std::string app_name_;
    std::string connect_token_;
    std::string connect_expires_;
    std::string connect_signature_;
    std::string stream_key_;
    bool is_publisher_{false};
    bool is_player_{false};
    std::uint32_t play_stream_id_{1};
};

}  // namespace otts::rtmp
