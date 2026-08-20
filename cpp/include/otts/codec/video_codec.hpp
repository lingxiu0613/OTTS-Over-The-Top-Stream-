#pragma once

#include "otts/media/stream_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace otts::codec {

struct ParameterSets {
    std::vector<std::uint8_t> vps;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;

    [[nodiscard]] bool complete(otts::media::CodecId codec) const;
    [[nodiscard]] std::vector<std::uint8_t> annexb() const;
};

struct FlvVideoPacket {
    otts::media::CodecId codec{otts::media::CodecId::Unknown};
    bool enhanced{false};
    bool sequence_header{false};
    bool coded_frames{false};
    bool keyframe{false};
    std::int32_t composition_time_ms{0};
    std::size_t data_offset{0};
};

std::optional<FlvVideoPacket> parse_flv_video_packet(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> annexb_to_length_prefixed(const std::vector<std::uint8_t>& annexb);
std::vector<std::uint8_t> length_prefixed_to_annexb(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t length_size = 4);
std::vector<std::uint8_t> length_prefixed_to_annexb(
    const std::vector<std::uint8_t>& data,
    std::size_t length_size = 4);
ParameterSets extract_parameter_sets(const std::vector<std::uint8_t>& annexb, otts::media::CodecId codec);
bool is_keyframe(const std::vector<std::uint8_t>& annexb, otts::media::CodecId codec);
std::vector<std::uint8_t> make_decoder_config(otts::media::CodecId codec, const ParameterSets& sets);
ParameterSets parse_decoder_config(otts::media::CodecId codec, const std::vector<std::uint8_t>& config);
std::vector<std::uint8_t> flv_video_config_to_annexb(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> flv_video_sample_to_annexb(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> make_flv_video_config(
    otts::media::CodecId codec,
    const ParameterSets& sets,
    bool enhanced_hevc = true);
std::vector<std::uint8_t> make_flv_video_sample(
    otts::media::CodecId codec,
    const std::vector<std::uint8_t>& annexb,
    bool keyframe,
    std::int32_t composition_time_ms = 0,
    bool enhanced_hevc = true);

}  // namespace otts::codec
