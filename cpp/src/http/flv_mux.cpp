#include "otts/http/flv_mux.hpp"

namespace otts::http {

namespace {

void write_be24(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

}  // namespace

std::vector<std::uint8_t> make_flv_file_header(bool has_audio, bool has_video) {
    std::vector<std::uint8_t> out;
    out.reserve(13);
    out.push_back('F');
    out.push_back('L');
    out.push_back('V');
    out.push_back(0x01);

    std::uint8_t flags = 0;
    if (has_audio) {
        flags |= 0x04;
    }
    if (has_video) {
        flags |= 0x01;
    }
    out.push_back(flags);

    write_be32(out, 9);
    write_be32(out, 0);
    return out;
}

std::vector<std::uint8_t> make_flv_tag(const otts::rtmp::MediaMessage& message) {
    std::vector<std::uint8_t> out;
    out.reserve(15 + message.payload.size());

    out.push_back(message.type_id);
    write_be24(out, static_cast<std::uint32_t>(message.payload.size()));
    write_be24(out, message.timestamp & 0xFFFFFF);
    out.push_back(static_cast<std::uint8_t>((message.timestamp >> 24) & 0xFF));
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);

    out.insert(out.end(), message.payload.begin(), message.payload.end());
    write_be32(out, static_cast<std::uint32_t>(11 + message.payload.size()));
    return out;
}

}  // namespace otts::http
