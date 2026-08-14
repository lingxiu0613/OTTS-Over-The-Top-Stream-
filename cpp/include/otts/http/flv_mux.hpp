#pragma once

#include "otts/rtmp/stream_registry.hpp"

#include <cstdint>
#include <vector>

namespace otts::http {

std::vector<std::uint8_t> make_flv_file_header(bool has_audio, bool has_video);
std::vector<std::uint8_t> make_flv_tag(const otts::rtmp::MediaMessage& message);

}  // namespace otts::http
