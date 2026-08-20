#include "otts/codec/video_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace otts::codec {
namespace {

constexpr std::array<std::uint8_t, 4> kStartCode{0, 0, 0, 1};

void write_be16(std::vector<std::uint8_t>& out, std::size_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void write_be32(std::vector<std::uint8_t>& out, std::size_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::optional<std::size_t> start_code_size(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) {
        return 4;
    }
    return std::nullopt;
}

template <typename Callback>
void for_each_annexb_nal(const std::vector<std::uint8_t>& data, Callback&& callback) {
    std::size_t cursor = 0;
    while (cursor < data.size()) {
        const auto code = start_code_size(data, cursor);
        if (!code) {
            ++cursor;
            continue;
        }
        const auto begin = cursor + *code;
        auto end = begin;
        while (end < data.size() && !start_code_size(data, end)) {
            ++end;
        }
        if (end > begin) {
            callback(data.data() + begin, end - begin);
        }
        cursor = end;
    }
}

std::uint8_t nal_type(const std::uint8_t* data, std::size_t size, otts::media::CodecId codec) {
    if (size == 0) {
        return 0xff;
    }
    if (codec == otts::media::CodecId::Hevc) {
        return static_cast<std::uint8_t>((data[0] >> 1) & 0x3f);
    }
    return static_cast<std::uint8_t>(data[0] & 0x1f);
}

std::int32_t read_signed_24(const std::uint8_t* data) {
    std::int32_t value = (static_cast<std::int32_t>(data[0]) << 16) |
                         (static_cast<std::int32_t>(data[1]) << 8) |
                         static_cast<std::int32_t>(data[2]);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<std::int32_t>(0xff000000);
    }
    return value;
}

void write_signed_24(std::vector<std::uint8_t>& out, std::int32_t value) {
    const auto encoded = static_cast<std::uint32_t>(value) & 0x00ffffffu;
    out.push_back(static_cast<std::uint8_t>((encoded >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((encoded >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(encoded & 0xff));
}

void append_hvcc_array(std::vector<std::uint8_t>& out, std::uint8_t type, const std::vector<std::uint8_t>& nal) {
    out.push_back(static_cast<std::uint8_t>(0x80 | type));
    write_be16(out, 1);
    write_be16(out, nal.size());
    out.insert(out.end(), nal.begin(), nal.end());
}

}  // namespace

bool ParameterSets::complete(otts::media::CodecId codec) const {
    return codec == otts::media::CodecId::Hevc
        ? !vps.empty() && !sps.empty() && !pps.empty()
        : !sps.empty() && !pps.empty();
}

std::vector<std::uint8_t> ParameterSets::annexb() const {
    std::vector<std::uint8_t> out;
    const auto append = [&](const std::vector<std::uint8_t>& nal) {
        if (!nal.empty()) {
            out.insert(out.end(), kStartCode.begin(), kStartCode.end());
            out.insert(out.end(), nal.begin(), nal.end());
        }
    };
    append(vps);
    append(sps);
    append(pps);
    return out;
}

std::optional<FlvVideoPacket> parse_flv_video_packet(const std::vector<std::uint8_t>& payload) {
    if (payload.empty()) {
        return std::nullopt;
    }
    FlvVideoPacket packet;
    if ((payload[0] & 0x80) != 0) {
        if (payload.size() < 5) {
            return std::nullopt;
        }
        packet.enhanced = true;
        packet.keyframe = ((payload[0] >> 4) & 0x07) == 1;
        const auto packet_type = static_cast<std::uint8_t>(payload[0] & 0x0f);
        if (payload[1] == 'h' && payload[2] == 'v' && payload[3] == 'c' && payload[4] == '1') {
            packet.codec = otts::media::CodecId::Hevc;
        } else if (payload[1] == 'a' && payload[2] == 'v' && payload[3] == 'c' && payload[4] == '1') {
            packet.codec = otts::media::CodecId::Avc;
        } else {
            return std::nullopt;
        }
        packet.sequence_header = packet_type == 0;
        packet.coded_frames = packet_type == 1 || packet_type == 3;
        packet.data_offset = 5;
        if (packet_type == 1) {
            if (payload.size() < 8) {
                return std::nullopt;
            }
            packet.composition_time_ms = read_signed_24(payload.data() + 5);
            packet.data_offset = 8;
        }
        return packet;
    }

    if (payload.size() < 5) {
        return std::nullopt;
    }
    const auto codec_id = static_cast<std::uint8_t>(payload[0] & 0x0f);
    if (codec_id == 7) {
        packet.codec = otts::media::CodecId::Avc;
    } else if (codec_id == 12) {
        packet.codec = otts::media::CodecId::Hevc;
    } else {
        return std::nullopt;
    }
    packet.keyframe = ((payload[0] >> 4) & 0x0f) == 1;
    packet.sequence_header = payload[1] == 0;
    packet.coded_frames = payload[1] == 1;
    packet.composition_time_ms = read_signed_24(payload.data() + 2);
    packet.data_offset = 5;
    return packet;
}

std::vector<std::uint8_t> annexb_to_length_prefixed(const std::vector<std::uint8_t>& annexb) {
    std::vector<std::uint8_t> out;
    for_each_annexb_nal(annexb, [&](const std::uint8_t* data, std::size_t size) {
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            return;
        }
        write_be32(out, size);
        out.insert(out.end(), data, data + size);
    });
    return out;
}

std::vector<std::uint8_t> length_prefixed_to_annexb(const std::uint8_t* data, std::size_t size, std::size_t length_size) {
    std::vector<std::uint8_t> out;
    if (length_size == 0 || length_size > 4) {
        return out;
    }
    std::size_t cursor = 0;
    while (cursor + length_size <= size) {
        std::size_t nal_size = 0;
        for (std::size_t i = 0; i < length_size; ++i) {
            nal_size = (nal_size << 8) | data[cursor + i];
        }
        cursor += length_size;
        if (nal_size == 0 || cursor + nal_size > size) {
            return {};
        }
        out.insert(out.end(), kStartCode.begin(), kStartCode.end());
        out.insert(out.end(), data + cursor, data + cursor + nal_size);
        cursor += nal_size;
    }
    return cursor == size ? out : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> length_prefixed_to_annexb(const std::vector<std::uint8_t>& data, std::size_t length_size) {
    return length_prefixed_to_annexb(data.data(), data.size(), length_size);
}

ParameterSets extract_parameter_sets(const std::vector<std::uint8_t>& annexb, otts::media::CodecId codec) {
    ParameterSets sets;
    for_each_annexb_nal(annexb, [&](const std::uint8_t* data, std::size_t size) {
        const auto type = nal_type(data, size, codec);
        if (codec == otts::media::CodecId::Hevc) {
            if (type == 32 && sets.vps.empty()) {
                sets.vps.assign(data, data + size);
            } else if (type == 33 && sets.sps.empty()) {
                sets.sps.assign(data, data + size);
            } else if (type == 34 && sets.pps.empty()) {
                sets.pps.assign(data, data + size);
            }
        } else if (type == 7 && sets.sps.empty()) {
            sets.sps.assign(data, data + size);
        } else if (type == 8 && sets.pps.empty()) {
            sets.pps.assign(data, data + size);
        }
    });
    return sets;
}

bool is_keyframe(const std::vector<std::uint8_t>& annexb, otts::media::CodecId codec) {
    bool keyframe = false;
    for_each_annexb_nal(annexb, [&](const std::uint8_t* data, std::size_t size) {
        const auto type = nal_type(data, size, codec);
        keyframe = keyframe || (codec == otts::media::CodecId::Hevc ? (type >= 16 && type <= 23) : type == 5);
    });
    return keyframe;
}

std::vector<std::uint8_t> make_decoder_config(otts::media::CodecId codec, const ParameterSets& sets) {
    if (!sets.complete(codec)) {
        return {};
    }
    std::vector<std::uint8_t> out;
    if (codec == otts::media::CodecId::Avc) {
        out = {1,
               sets.sps.size() > 1 ? sets.sps[1] : static_cast<std::uint8_t>(0x64),
               sets.sps.size() > 2 ? sets.sps[2] : static_cast<std::uint8_t>(0),
               sets.sps.size() > 3 ? sets.sps[3] : static_cast<std::uint8_t>(0x1f),
               0xff, 0xe1};
        write_be16(out, sets.sps.size());
        out.insert(out.end(), sets.sps.begin(), sets.sps.end());
        out.push_back(1);
        write_be16(out, sets.pps.size());
        out.insert(out.end(), sets.pps.begin(), sets.pps.end());
        return out;
    }
    if (codec != otts::media::CodecId::Hevc) {
        return {};
    }

    out = {
        1, 1, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        120, 0xf0, 0x00, 0xfc, 0xfd, 0xf8, 0xf8,
        0, 0, 0x0f, 3
    };
    // VPS carries general_profile_* and general_level_idc consecutively in
    // the common single-layer profile_tier_level layout emitted by FFmpeg,
    // x265 and OBS. Keep hvcC consistent with the actual elementary stream.
    if (sets.vps.size() >= 18) {
        std::copy_n(sets.vps.begin() + 6, 12, out.begin() + 1);
    }
    append_hvcc_array(out, 32, sets.vps);
    append_hvcc_array(out, 33, sets.sps);
    append_hvcc_array(out, 34, sets.pps);
    return out;
}

ParameterSets parse_decoder_config(otts::media::CodecId codec, const std::vector<std::uint8_t>& config) {
    ParameterSets sets;
    if (codec == otts::media::CodecId::Avc) {
        if (config.size() < 7) {
            return sets;
        }
        std::size_t cursor = 5;
        const auto sps_count = static_cast<std::size_t>(config[cursor++] & 0x1f);
        for (std::size_t i = 0; i < sps_count && cursor + 2 <= config.size(); ++i) {
            const auto size = static_cast<std::size_t>((config[cursor] << 8) | config[cursor + 1]);
            cursor += 2;
            if (cursor + size > config.size()) {
                return {};
            }
            if (sets.sps.empty()) {
                sets.sps.assign(config.begin() + static_cast<std::ptrdiff_t>(cursor), config.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            }
            cursor += size;
        }
        if (cursor >= config.size()) {
            return {};
        }
        const auto pps_count = config[cursor++];
        for (std::size_t i = 0; i < pps_count && cursor + 2 <= config.size(); ++i) {
            const auto size = static_cast<std::size_t>((config[cursor] << 8) | config[cursor + 1]);
            cursor += 2;
            if (cursor + size > config.size()) {
                return {};
            }
            if (sets.pps.empty()) {
                sets.pps.assign(config.begin() + static_cast<std::ptrdiff_t>(cursor), config.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            }
            cursor += size;
        }
        return sets;
    }
    if (codec != otts::media::CodecId::Hevc || config.size() < 23) {
        return sets;
    }
    std::size_t cursor = 23;
    const auto arrays = config[22];
    for (std::size_t i = 0; i < arrays && cursor + 3 <= config.size(); ++i) {
        const auto type = config[cursor++] & 0x3f;
        const auto count = static_cast<std::size_t>((config[cursor] << 8) | config[cursor + 1]);
        cursor += 2;
        for (std::size_t n = 0; n < count && cursor + 2 <= config.size(); ++n) {
            const auto size = static_cast<std::size_t>((config[cursor] << 8) | config[cursor + 1]);
            cursor += 2;
            if (cursor + size > config.size()) {
                return {};
            }
            auto* target = type == 32 ? &sets.vps : type == 33 ? &sets.sps : type == 34 ? &sets.pps : nullptr;
            if (target != nullptr && target->empty()) {
                target->assign(config.begin() + static_cast<std::ptrdiff_t>(cursor), config.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            }
            cursor += size;
        }
    }
    return sets;
}

std::vector<std::uint8_t> flv_video_config_to_annexb(const std::vector<std::uint8_t>& payload) {
    const auto packet = parse_flv_video_packet(payload);
    if (!packet || !packet->sequence_header || packet->data_offset >= payload.size()) {
        return {};
    }
    const std::vector<std::uint8_t> config(payload.begin() + static_cast<std::ptrdiff_t>(packet->data_offset), payload.end());
    return parse_decoder_config(packet->codec, config).annexb();
}

std::vector<std::uint8_t> flv_video_sample_to_annexb(const std::vector<std::uint8_t>& payload) {
    const auto packet = parse_flv_video_packet(payload);
    if (!packet || !packet->coded_frames || packet->data_offset >= payload.size()) {
        return {};
    }
    const auto* data = payload.data() + packet->data_offset;
    const auto size = payload.size() - packet->data_offset;
    if (size >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        return std::vector<std::uint8_t>(data, data + size);
    }
    return length_prefixed_to_annexb(data, size);
}

std::vector<std::uint8_t> make_flv_video_config(otts::media::CodecId codec, const ParameterSets& sets, bool enhanced_hevc) {
    const auto config = make_decoder_config(codec, sets);
    if (config.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out;
    if (codec == otts::media::CodecId::Hevc && enhanced_hevc) {
        out = {0x90, 'h', 'v', 'c', '1'};
    } else {
        out = {static_cast<std::uint8_t>(0x10 | (codec == otts::media::CodecId::Hevc ? 12 : 7)), 0, 0, 0, 0};
    }
    out.insert(out.end(), config.begin(), config.end());
    return out;
}

std::vector<std::uint8_t> make_flv_video_sample(
    otts::media::CodecId codec,
    const std::vector<std::uint8_t>& annexb,
    bool keyframe,
    std::int32_t composition_time_ms,
    bool enhanced_hevc) {
    // AVC/HEVC coded-frame payloads use ISO BMFF length-prefixed NAL units.
    // Keep Annex-B acceptance in the parser for tolerant ingest, but never emit
    // it in Enhanced RTMP because standards-compliant demuxers read a NAL size.
    auto sample = annexb_to_length_prefixed(annexb);
    if (sample.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out;
    if (codec == otts::media::CodecId::Hevc && enhanced_hevc) {
        out.push_back(static_cast<std::uint8_t>(0x80 | (keyframe ? 0x10 : 0x20) | 1));
        out.insert(out.end(), {'h', 'v', 'c', '1'});
        write_signed_24(out, composition_time_ms);
    } else {
        out.push_back(static_cast<std::uint8_t>((keyframe ? 0x10 : 0x20) | (codec == otts::media::CodecId::Hevc ? 12 : 7)));
        out.push_back(1);
        write_signed_24(out, composition_time_ms);
    }
    out.insert(out.end(), sample.begin(), sample.end());
    return out;
}

}  // namespace otts::codec
