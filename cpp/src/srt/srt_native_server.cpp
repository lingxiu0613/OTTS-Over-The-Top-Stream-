#include "otts/srt/srt_native_server.hpp"

#include "otts/core/logger.hpp"

#include <srt/srt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>

namespace otts::srt {
namespace {

constexpr std::uint16_t kPatPid = 0x0000;
constexpr std::uint16_t kPmtPid = 0x0100;
constexpr std::uint16_t kVideoPid = 0x0101;
constexpr std::uint16_t kAudioPid = 0x0102;
constexpr std::uint8_t kVideoStreamId = 0xe0;
constexpr std::uint8_t kAudioStreamId = 0xc0;

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

otts::rtmp::StreamRegistry::CallbackId next_callback_id() {
    static std::atomic<otts::rtmp::StreamRegistry::CallbackId> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t read_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void write_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint32_t crc32_mpeg(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04c11db7u) : (crc << 1);
        }
    }
    return crc;
}

std::vector<std::uint8_t> annexb_to_avcc(const std::vector<std::uint8_t>& annexb) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    auto start_code = [&](std::size_t pos, std::size_t& len) {
        if (pos + 3 <= annexb.size() && annexb[pos] == 0 && annexb[pos + 1] == 0 && annexb[pos + 2] == 1) {
            len = 3;
            return true;
        }
        if (pos + 4 <= annexb.size() && annexb[pos] == 0 && annexb[pos + 1] == 0 && annexb[pos + 2] == 0 && annexb[pos + 3] == 1) {
            len = 4;
            return true;
        }
        return false;
    };
    while (i < annexb.size()) {
        std::size_t sc_len = 0;
        if (!start_code(i, sc_len)) {
            ++i;
            continue;
        }
        const auto nal_start = i + sc_len;
        auto nal_end = nal_start;
        while (nal_end < annexb.size()) {
            std::size_t next_len = 0;
            if (start_code(nal_end, next_len)) {
                break;
            }
            ++nal_end;
        }
        const auto nal_size = nal_end - nal_start;
        if (nal_size > 0) {
            write_be32(out, static_cast<std::uint32_t>(nal_size));
            out.insert(out.end(), annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
        }
        i = nal_end;
    }
    return out;
}

struct NalSets {
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

NalSets extract_parameter_sets(const std::vector<std::uint8_t>& annexb) {
    NalSets sets;
    std::size_t i = 0;
    auto start_code = [&](std::size_t pos, std::size_t& len) {
        if (pos + 3 <= annexb.size() && annexb[pos] == 0 && annexb[pos + 1] == 0 && annexb[pos + 2] == 1) {
            len = 3;
            return true;
        }
        if (pos + 4 <= annexb.size() && annexb[pos] == 0 && annexb[pos + 1] == 0 && annexb[pos + 2] == 0 && annexb[pos + 3] == 1) {
            len = 4;
            return true;
        }
        return false;
    };
    while (i < annexb.size()) {
        std::size_t sc_len = 0;
        if (!start_code(i, sc_len)) {
            ++i;
            continue;
        }
        const auto nal_start = i + sc_len;
        auto nal_end = nal_start;
        while (nal_end < annexb.size()) {
            std::size_t next_len = 0;
            if (start_code(nal_end, next_len)) {
                break;
            }
            ++nal_end;
        }
        if (nal_end > nal_start) {
            const auto nal_type = annexb[nal_start] & 0x1f;
            if (nal_type == 7 && sets.sps.empty()) {
                sets.sps.assign(annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
            } else if (nal_type == 8 && sets.pps.empty()) {
                sets.pps.assign(annexb.begin() + static_cast<std::ptrdiff_t>(nal_start), annexb.begin() + static_cast<std::ptrdiff_t>(nal_end));
            }
        }
        i = nal_end;
    }
    return sets;
}

bool has_idr(const std::vector<std::uint8_t>& annexb) {
    for (std::size_t i = 0; i + 4 < annexb.size(); ++i) {
        bool code3 = annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1;
        bool code4 = i + 4 < annexb.size() && annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1;
        if (code3 || code4) {
            const auto nal = i + (code3 ? 3 : 4);
            if (nal < annexb.size() && (annexb[nal] & 0x1f) == 5) {
                return true;
            }
        }
    }
    return false;
}

otts::rtmp::MediaMessage make_avc_sequence_header(std::uint32_t timestamp, const NalSets& sets) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload = {0x17, 0x00, 0x00, 0x00, 0x00};
    message.payload.push_back(0x01);
    message.payload.push_back(sets.sps.size() > 1 ? sets.sps[1] : 0x64);
    message.payload.push_back(sets.sps.size() > 2 ? sets.sps[2] : 0x00);
    message.payload.push_back(sets.sps.size() > 3 ? sets.sps[3] : 0x1f);
    message.payload.push_back(0xff);
    message.payload.push_back(0xe1);
    write_be16(message.payload, static_cast<std::uint16_t>(sets.sps.size()));
    message.payload.insert(message.payload.end(), sets.sps.begin(), sets.sps.end());
    message.payload.push_back(0x01);
    write_be16(message.payload, static_cast<std::uint16_t>(sets.pps.size()));
    message.payload.insert(message.payload.end(), sets.pps.begin(), sets.pps.end());
    return message;
}

otts::rtmp::MediaMessage make_avc_nalu_message(std::uint32_t timestamp, const std::vector<std::uint8_t>& annexb, bool keyframe) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 9;
    message.message_stream_id = 1;
    message.payload.push_back(static_cast<std::uint8_t>((keyframe ? 0x10 : 0x20) | 0x07));
    message.payload.push_back(0x01);
    message.payload.push_back(0x00);
    message.payload.push_back(0x00);
    message.payload.push_back(0x00);
    auto avcc = annexb_to_avcc(annexb);
    message.payload.insert(message.payload.end(), avcc.begin(), avcc.end());
    return message;
}

std::vector<std::uint8_t> flv_video_to_annexb(const otts::rtmp::MediaMessage& message) {
    std::vector<std::uint8_t> out;
    if (message.type_id != 9 || message.payload.size() < 5) {
        return out;
    }
    if ((message.payload[0] & 0x0f) != 7 || message.payload[1] != 1) {
        return out;
    }
    std::size_t cursor = 5;
    while (cursor + 4 <= message.payload.size()) {
        const auto size = read_be32(message.payload.data() + cursor);
        cursor += 4;
        if (size == 0 || cursor + size > message.payload.size()) {
            break;
        }
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(out.end(), message.payload.begin() + static_cast<std::ptrdiff_t>(cursor), message.payload.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += size;
    }
    return out;
}

std::vector<std::uint8_t> flv_avc_config_to_annexb(const otts::rtmp::MediaMessage& message) {
    std::vector<std::uint8_t> out;
    if (message.type_id != 9 || message.payload.size() < 12 ||
        (message.payload[0] & 0x0f) != 7 || message.payload[1] != 0) {
        return out;
    }

    std::size_t cursor = 10;
    const auto append_nalus = [&](std::size_t count, std::size_t& position) -> bool {
        for (std::size_t i = 0; i < count; ++i) {
            if (position + 2 > message.payload.size()) {
                return false;
            }
            const auto size = static_cast<std::size_t>(
                (static_cast<std::uint16_t>(message.payload[position]) << 8) |
                message.payload[position + 1]);
            position += 2;
            if (size == 0 || position + size > message.payload.size()) {
                return false;
            }
            out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
            out.insert(
                out.end(),
                message.payload.begin() + static_cast<std::ptrdiff_t>(position),
                message.payload.begin() + static_cast<std::ptrdiff_t>(position + size));
            position += size;
        }
        return true;
    };

    const auto sps_count = static_cast<std::size_t>(message.payload[cursor++] & 0x1f);
    if (!append_nalus(sps_count, cursor) || cursor >= message.payload.size()) {
        return {};
    }
    const auto pps_count = static_cast<std::size_t>(message.payload[cursor++]);
    if (!append_nalus(pps_count, cursor) || sps_count == 0 || pps_count == 0) {
        return {};
    }
    return out;
}

struct AacConfig {
    std::uint8_t profile{2};
    std::uint8_t sampling_frequency_index{4};
    std::uint8_t channel_config{2};
    bool valid{false};
};

AacConfig parse_adts_config(const std::vector<std::uint8_t>& payload) {
    AacConfig config;
    if (payload.size() < 7 || payload[0] != 0xff || (payload[1] & 0xf0) != 0xf0) {
        return config;
    }
    config.profile = static_cast<std::uint8_t>(((payload[2] & 0xc0) >> 6) + 1);
    config.sampling_frequency_index = static_cast<std::uint8_t>((payload[2] & 0x3c) >> 2);
    config.channel_config = static_cast<std::uint8_t>(((payload[2] & 0x01) << 2) | ((payload[3] & 0xc0) >> 6));
    config.valid = true;
    return config;
}

std::size_t adts_frame_length(const std::uint8_t* data, std::size_t size) {
    if (size < 7 || data[0] != 0xff || (data[1] & 0xf0) != 0xf0) {
        return 0;
    }
    return ((static_cast<std::size_t>(data[3] & 0x03) << 11) |
            (static_cast<std::size_t>(data[4]) << 3) |
            ((data[5] & 0xe0) >> 5));
}

otts::rtmp::MediaMessage make_aac_sequence_header(std::uint32_t timestamp, const AacConfig& config) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 8;
    message.message_stream_id = 1;
    message.payload.push_back(0xaf);
    message.payload.push_back(0x00);
    message.payload.push_back(static_cast<std::uint8_t>((config.profile << 3) | ((config.sampling_frequency_index >> 1) & 0x07)));
    message.payload.push_back(static_cast<std::uint8_t>(((config.sampling_frequency_index & 0x01) << 7) | ((config.channel_config & 0x0f) << 3)));
    return message;
}

otts::rtmp::MediaMessage make_aac_raw_message(std::uint32_t timestamp, const std::uint8_t* data, std::size_t size) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp;
    message.type_id = 8;
    message.message_stream_id = 1;
    message.payload.push_back(0xaf);
    message.payload.push_back(0x01);
    message.payload.insert(message.payload.end(), data, data + size);
    return message;
}

AacConfig parse_flv_aac_config(const otts::rtmp::MediaMessage& message) {
    AacConfig config;
    if (message.type_id != 8 || message.payload.size() < 4 || (message.payload[0] >> 4) != 10 || message.payload[1] != 0) {
        return config;
    }
    config.profile = static_cast<std::uint8_t>((message.payload[2] >> 3) & 0x1f);
    config.sampling_frequency_index = static_cast<std::uint8_t>(((message.payload[2] & 0x07) << 1) | ((message.payload[3] >> 7) & 0x01));
    config.channel_config = static_cast<std::uint8_t>((message.payload[3] >> 3) & 0x0f);
    config.valid = config.profile != 0;
    return config;
}

std::vector<std::uint8_t> make_adts_frame(const otts::rtmp::MediaMessage& message, const AacConfig& config) {
    std::vector<std::uint8_t> frame;
    if (!config.valid || message.type_id != 8 || message.payload.size() <= 2 || (message.payload[0] >> 4) != 10 || message.payload[1] != 1) {
        return frame;
    }
    const auto raw_size = message.payload.size() - 2;
    const auto frame_length = raw_size + 7;
    frame.resize(7);
    const auto profile_minus_one = static_cast<std::uint8_t>(config.profile > 0 ? config.profile - 1 : 1);
    frame[0] = 0xff;
    frame[1] = 0xf1;
    frame[2] = static_cast<std::uint8_t>(((profile_minus_one & 0x03) << 6) |
                                         ((config.sampling_frequency_index & 0x0f) << 2) |
                                         ((config.channel_config >> 2) & 0x01));
    frame[3] = static_cast<std::uint8_t>(((config.channel_config & 0x03) << 6) | ((frame_length >> 11) & 0x03));
    frame[4] = static_cast<std::uint8_t>((frame_length >> 3) & 0xff);
    frame[5] = static_cast<std::uint8_t>(((frame_length & 0x07) << 5) | 0x1f);
    frame[6] = 0xfc;
    frame.insert(frame.end(), message.payload.begin() + 2, message.payload.end());
    return frame;
}

std::vector<std::uint8_t> make_pat(std::uint8_t continuity) {
    std::vector<std::uint8_t> section = {
        0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x00, 0x01, static_cast<std::uint8_t>(0xe0 | ((kPmtPid >> 8) & 0x1f)), static_cast<std::uint8_t>(kPmtPid & 0xff)
    };
    const auto crc = crc32_mpeg(section.data(), section.size());
    write_be32(section, crc);
    std::vector<std::uint8_t> packet(188, 0xff);
    packet[0] = 0x47;
    packet[1] = 0x40;
    packet[2] = 0x00;
    packet[3] = static_cast<std::uint8_t>(0x10 | (continuity & 0x0f));
    packet[4] = 0x00;
    std::copy(section.begin(), section.end(), packet.begin() + 5);
    return packet;
}

std::vector<std::uint8_t> make_pmt(std::uint8_t continuity) {
    std::vector<std::uint8_t> section = {
        0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
        static_cast<std::uint8_t>(0xe0 | ((kVideoPid >> 8) & 0x1f)), static_cast<std::uint8_t>(kVideoPid & 0xff),
        0xf0, 0x00,
        0x1b, static_cast<std::uint8_t>(0xe0 | ((kVideoPid >> 8) & 0x1f)), static_cast<std::uint8_t>(kVideoPid & 0xff), 0xf0, 0x00,
        0x0f, static_cast<std::uint8_t>(0xe0 | ((kAudioPid >> 8) & 0x1f)), static_cast<std::uint8_t>(kAudioPid & 0xff), 0xf0, 0x00
    };
    const auto crc = crc32_mpeg(section.data(), section.size());
    write_be32(section, crc);
    std::vector<std::uint8_t> packet(188, 0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>(0x40 | ((kPmtPid >> 8) & 0x1f));
    packet[2] = static_cast<std::uint8_t>(kPmtPid & 0xff);
    packet[3] = static_cast<std::uint8_t>(0x10 | (continuity & 0x0f));
    packet[4] = 0x00;
    std::copy(section.begin(), section.end(), packet.begin() + 5);
    return packet;
}

void write_pts(std::vector<std::uint8_t>& out, std::uint8_t prefix, std::uint64_t pts) {
    pts &= 0x1ffffffffull;
    out.push_back(static_cast<std::uint8_t>((prefix << 4) | (((pts >> 30) & 0x07) << 1) | 1));
    out.push_back(static_cast<std::uint8_t>((pts >> 22) & 0xff));
    out.push_back(static_cast<std::uint8_t>((((pts >> 15) & 0x7f) << 1) | 1));
    out.push_back(static_cast<std::uint8_t>((pts >> 7) & 0xff));
    out.push_back(static_cast<std::uint8_t>(((pts & 0x7f) << 1) | 1));
}

std::vector<std::uint8_t> make_pes_video(const std::vector<std::uint8_t>& annexb, std::uint32_t timestamp_ms) {
    std::vector<std::uint8_t> pes;
    pes.insert(pes.end(), {0x00, 0x00, 0x01, kVideoStreamId});
    write_be16(pes, 0x0000);
    pes.push_back(0x80);
    pes.push_back(0x80);
    pes.push_back(0x05);
    write_pts(pes, 0x02, static_cast<std::uint64_t>(timestamp_ms) * 90);
    pes.insert(pes.end(), annexb.begin(), annexb.end());
    return pes;
}

std::vector<std::uint8_t> make_pes_audio(const std::vector<std::uint8_t>& adts, std::uint32_t timestamp_ms) {
    std::vector<std::uint8_t> pes;
    const auto pes_size = std::min<std::size_t>(0xffff, adts.size() + 8);
    pes.insert(pes.end(), {0x00, 0x00, 0x01, kAudioStreamId});
    write_be16(pes, static_cast<std::uint16_t>(pes_size));
    pes.push_back(0x80);
    pes.push_back(0x80);
    pes.push_back(0x05);
    write_pts(pes, 0x02, static_cast<std::uint64_t>(timestamp_ms) * 90);
    pes.insert(pes.end(), adts.begin(), adts.end());
    return pes;
}

std::vector<std::uint8_t> packetize_ts(std::uint16_t pid, const std::vector<std::uint8_t>& payload, std::uint8_t& continuity) {
    std::vector<std::uint8_t> out;
    std::size_t cursor = 0;
    bool first = true;
    while (cursor < payload.size()) {
        std::array<std::uint8_t, 188> packet{};
        packet.fill(0xff);
        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1f));
        packet[2] = static_cast<std::uint8_t>(pid & 0xff);
        const auto remaining = payload.size() - cursor;
        if (remaining < 184) {
            const auto stuff = 184 - remaining;
            packet[3] = static_cast<std::uint8_t>(0x30 | (continuity & 0x0f));
            packet[4] = static_cast<std::uint8_t>(stuff - 1);
            if (stuff > 1) {
                packet[5] = 0x00;
            }
            const auto payload_start = 4 + stuff;
            std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(payload_start));
            cursor = payload.size();
        } else {
            packet[3] = static_cast<std::uint8_t>(0x10 | (continuity & 0x0f));
            std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.begin() + static_cast<std::ptrdiff_t>(cursor + 184), packet.begin() + 4);
            cursor += 184;
        }
        continuity = static_cast<std::uint8_t>((continuity + 1) & 0x0f);
        out.insert(out.end(), packet.begin(), packet.end());
        first = false;
    }
    return out;
}

struct PesPacket {
    std::uint16_t pid{0};
    std::uint8_t stream_id{0};
    std::uint32_t timestamp_ms{0};
    std::vector<std::uint8_t> payload;
};

class TsDemuxer {
public:
    std::vector<PesPacket> push(const std::uint8_t* data, std::size_t size) {
        std::vector<PesPacket> packets;
        buffer_.insert(buffer_.end(), data, data + size);
        while (buffer_.size() >= 188) {
            auto sync = std::find(buffer_.begin(), buffer_.end(), 0x47);
            if (sync != buffer_.begin()) {
                buffer_.erase(buffer_.begin(), sync);
                if (buffer_.size() < 188) {
                    break;
                }
            }
            std::array<std::uint8_t, 188> packet{};
            std::copy(buffer_.begin(), buffer_.begin() + 188, packet.begin());
            buffer_.erase(buffer_.begin(), buffer_.begin() + 188);
            parse_ts_packet(packet.data(), packets);
        }
        return packets;
    }

    std::vector<PesPacket> flush() {
        std::vector<PesPacket> packets;
        for (auto& [pid, state] : pes_) {
            (void)pid;
            emit_pes(state, packets);
        }
        pes_.clear();
        return packets;
    }

private:
    struct PesState {
        std::uint16_t pid{0};
        std::vector<std::uint8_t> data;
    };

    void parse_ts_packet(const std::uint8_t* packet, std::vector<PesPacket>& out) {
        if (packet[0] != 0x47 || (packet[1] & 0x80)) {
            return;
        }
        const bool payload_start = (packet[1] & 0x40) != 0;
        const auto pid = static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
        const auto adaptation = (packet[3] >> 4) & 0x03;
        std::size_t cursor = 4;
        if (adaptation == 0 || adaptation == 2) {
            return;
        }
        if (adaptation == 3) {
            const auto length = packet[cursor++];
            cursor += length;
            if (cursor >= 188) {
                return;
            }
        }
        if (pid == kPatPid) {
            return;
        }
        auto& state = pes_[pid];
        state.pid = pid;
        if (payload_start && !state.data.empty()) {
            emit_pes(state, out);
            state.data.clear();
        }
        state.data.insert(state.data.end(), packet + cursor, packet + 188);
    }

    void emit_pes(PesState& state, std::vector<PesPacket>& out) {
        auto& data = state.data;
        if (data.size() < 9 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) {
            return;
        }
        PesPacket packet;
        packet.pid = state.pid;
        packet.stream_id = data[3];
        const auto flags = data[7];
        const auto header_len = data[8];
        const auto payload_start = static_cast<std::size_t>(9 + header_len);
        if (payload_start > data.size()) {
            return;
        }
        if ((flags & 0x80) && header_len >= 5) {
            const auto* p = data.data() + 9;
            const std::uint64_t pts =
                ((static_cast<std::uint64_t>((p[0] >> 1) & 0x07)) << 30) |
                (static_cast<std::uint64_t>(p[1]) << 22) |
                ((static_cast<std::uint64_t>((p[2] >> 1) & 0x7f)) << 15) |
                (static_cast<std::uint64_t>(p[3]) << 7) |
                static_cast<std::uint64_t>((p[4] >> 1) & 0x7f);
            packet.timestamp_ms = static_cast<std::uint32_t>(pts / 90);
        }
        packet.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(payload_start), data.end());
        while (!packet.payload.empty() && packet.payload.back() == 0xff) {
            packet.payload.pop_back();
        }
        out.push_back(std::move(packet));
    }

    std::vector<std::uint8_t> buffer_;
    std::unordered_map<std::uint16_t, PesState> pes_;
};

class H264TsExtractor {
public:
    std::vector<PesPacket> push(const std::uint8_t* data, std::size_t size) {
        std::vector<PesPacket> out;
        buffer_.insert(buffer_.end(), data, data + size);
        while (buffer_.size() >= 188) {
            auto sync = std::find(buffer_.begin(), buffer_.end(), 0x47);
            if (sync != buffer_.begin()) {
                buffer_.erase(buffer_.begin(), sync);
                if (buffer_.size() < 188) {
                    break;
                }
            }
            std::array<std::uint8_t, 188> packet{};
            std::copy(buffer_.begin(), buffer_.begin() + 188, packet.begin());
            buffer_.erase(buffer_.begin(), buffer_.begin() + 188);
            parse_packet(packet.data(), out);
        }
        extract_access_units(out);
        return out;
    }

    std::vector<PesPacket> flush() {
        std::vector<PesPacket> out;
        if (!es_.empty()) {
            PesPacket packet;
            packet.pid = video_pid_.value_or(0);
            packet.stream_id = kVideoStreamId;
            packet.timestamp_ms = next_timestamp_ms_;
            packet.payload = std::move(es_);
            es_.clear();
            out.push_back(std::move(packet));
        }
        return out;
    }

private:
    static std::optional<std::size_t> start_code_at(const std::vector<std::uint8_t>& data, std::size_t pos) {
        if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
            return 3;
        }
        if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) {
            return 4;
        }
        return std::nullopt;
    }

    static bool is_aud_at(const std::vector<std::uint8_t>& data, std::size_t pos) {
        const auto code = start_code_at(data, pos);
        if (!code.has_value()) {
            return false;
        }
        const auto nal = pos + *code;
        return nal < data.size() && (data[nal] & 0x1f) == 9;
    }

    static std::optional<std::uint32_t> parse_pts_ms(const std::uint8_t* payload, std::size_t size) {
        if (size < 14 || payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
            return std::nullopt;
        }
        if ((payload[7] & 0x80) == 0 || payload[8] < 5 || size < 14) {
            return std::nullopt;
        }
        const auto* p = payload + 9;
        const std::uint64_t pts =
            ((static_cast<std::uint64_t>((p[0] >> 1) & 0x07)) << 30) |
            (static_cast<std::uint64_t>(p[1]) << 22) |
            ((static_cast<std::uint64_t>((p[2] >> 1) & 0x7f)) << 15) |
            (static_cast<std::uint64_t>(p[3]) << 7) |
            static_cast<std::uint64_t>((p[4] >> 1) & 0x7f);
        return static_cast<std::uint32_t>(pts / 90);
    }

    void parse_packet(const std::uint8_t* packet, std::vector<PesPacket>& out) {
        (void)out;
        if (packet[0] != 0x47 || (packet[1] & 0x80)) {
            return;
        }
        const bool payload_start = (packet[1] & 0x40) != 0;
        const auto pid = static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
        const auto adaptation = (packet[3] >> 4) & 0x03;
        std::size_t cursor = 4;
        if (adaptation == 0 || adaptation == 2) {
            return;
        }
        if (adaptation == 3) {
            const auto length = packet[cursor++];
            cursor += length;
            if (cursor >= 188) {
                return;
            }
        }
        if (pid == kPatPid) {
            return;
        }
        if (payload_start && cursor + 9 < 188 && packet[cursor] == 0x00 && packet[cursor + 1] == 0x00 && packet[cursor + 2] == 0x01) {
            const auto stream_id = packet[cursor + 3];
            if ((stream_id & 0xf0) == 0xe0) {
                video_pid_ = pid;
                if (const auto pts = parse_pts_ms(packet + cursor, 188 - cursor)) {
                    next_timestamp_ms_ = *pts;
                }
                const auto header_len = packet[cursor + 8];
                cursor += 9 + header_len;
            }
        }
        if (video_pid_.has_value() && pid == *video_pid_ && cursor < 188) {
            es_.insert(es_.end(), packet + cursor, packet + 188);
        }
    }

    void extract_access_units(std::vector<PesPacket>& out) {
        std::vector<std::size_t> auds;
        for (std::size_t i = 0; i + 5 < es_.size(); ++i) {
            if (is_aud_at(es_, i)) {
                auds.push_back(i);
            }
        }
        while (auds.size() >= 2) {
            const auto begin = auds[0];
            const auto end = auds[1];
            if (end > begin) {
                PesPacket packet;
                packet.pid = video_pid_.value_or(0);
                packet.stream_id = kVideoStreamId;
                packet.timestamp_ms = next_timestamp_ms_;
                packet.payload.assign(es_.begin() + static_cast<std::ptrdiff_t>(begin), es_.begin() + static_cast<std::ptrdiff_t>(end));
                out.push_back(std::move(packet));
                next_timestamp_ms_ += 40;
            }
            es_.erase(es_.begin(), es_.begin() + static_cast<std::ptrdiff_t>(end));
            auds.erase(auds.begin());
            for (auto& pos : auds) {
                pos -= end;
            }
        }
        if (es_.size() > 1024 * 1024) {
            es_.erase(es_.begin(), es_.end() - 256 * 1024);
        }
    }

    std::vector<std::uint8_t> buffer_;
    std::vector<std::uint8_t> es_;
    std::optional<std::uint16_t> video_pid_;
    std::uint32_t next_timestamp_ms_{0};
};

int make_listener(std::uint16_t port) {
    const auto sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        return -1;
    }
    int yes = 1;
    srt_setsockopt(sock, 0, SRTO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (srt_bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SRT_ERROR) {
        srt_close(sock);
        return -1;
    }
    if (srt_listen(sock, 16) == SRT_ERROR) {
        srt_close(sock);
        return -1;
    }
    return sock;
}

std::string trim_copy(std::string value) {
    if (const auto null_pos = value.find('\0'); null_pos != std::string::npos) {
        value.resize(null_pos);
    }
    const auto first = value.find_first_not_of(" \t\r\n\0", 0);
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n\0");
    return value.substr(first, last - first + 1);
}

std::string normalize_stream_key(std::string value) {
    value = trim_copy(std::move(value));
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    if (value.size() > 4 && value.substr(value.size() - 4) == ".sdp") {
        value.resize(value.size() - 4);
    }
    const auto separator = value.find("__");
    if (separator != std::string::npos && value.find('/') == std::string::npos) {
        value.replace(separator, 2, "/");
    }
    return value;
}

std::string parse_stream_id(std::string stream_id) {
    stream_id = trim_copy(std::move(stream_id));
    if (stream_id.empty()) {
        return {};
    }
    if (stream_id.rfind("#!::", 0) == 0) {
        stream_id.erase(0, 4);
        std::stringstream fields(stream_id);
        std::string field;
        while (std::getline(fields, field, ',')) {
            if (field.rfind("r=", 0) == 0 || field.rfind("stream=", 0) == 0) {
                const auto equals = field.find('=');
                return normalize_stream_key(field.substr(equals + 1));
            }
        }
        return {};
    }
    if (stream_id.find("app=") != std::string::npos && stream_id.find("stream=") != std::string::npos) {
        std::string app;
        std::string name;
        std::stringstream fields(stream_id);
        std::string field;
        while (std::getline(fields, field, '&')) {
            if (field.rfind("app=", 0) == 0) {
                app = field.substr(4);
            } else if (field.rfind("stream=", 0) == 0) {
                name = field.substr(7);
            }
        }
        if (!app.empty() && !name.empty()) {
            return normalize_stream_key(app + "/" + name);
        }
    }
    return normalize_stream_key(std::move(stream_id));
}

std::string stream_key_from_socket(int socket, const std::string& fallback) {
    std::array<char, 512> buffer{};
    int length = static_cast<int>(buffer.size());
    if (srt_getsockflag(socket, SRTO_STREAMID, buffer.data(), &length) != SRT_ERROR && length > 0) {
        auto stream_key = parse_stream_id(std::string(buffer.data(), static_cast<std::size_t>(length)));
        if (!stream_key.empty()) {
            return stream_key;
        }
    }
    return fallback.empty() ? std::string("live/srt-demo") : fallback;
}

}  // namespace

SrtNativeServer::SrtNativeServer(
    std::uint16_t publish_port,
    std::uint16_t play_port,
    std::string publish_stream_key,
    otts::rtmp::StreamRegistry& registry)
    : publish_port_(publish_port),
      play_port_(play_port),
      publish_stream_key_(std::move(publish_stream_key)),
      registry_(registry) {}

SrtNativeServer::~SrtNativeServer() {
    stop();
}

bool SrtNativeServer::start() {
    if (publish_port_ == 0 && play_port_ == 0) {
        return true;
    }
    if (srt_startup() == SRT_ERROR) {
        otts::core::log_error("srt_native", "srt_startup failed");
        return false;
    }
    running_.store(true);
    if (publish_port_ != 0) {
        publish_socket_ = make_listener(publish_port_);
        if (publish_socket_ == -1) {
            otts::core::log_error("srt_native", "publish bind failed on " + std::to_string(publish_port_));
            return false;
        }
        publish_thread_ = std::thread(&SrtNativeServer::publish_loop, this);
        otts::core::log_info("srt_native", "C++ SRT publish listening on 0.0.0.0:" + std::to_string(publish_port_));
    }
    if (play_port_ != 0) {
        play_socket_ = make_listener(play_port_);
        if (play_socket_ == -1) {
            otts::core::log_error("srt_native", "play bind failed on " + std::to_string(play_port_));
            return false;
        }
        play_thread_ = std::thread(&SrtNativeServer::play_loop, this);
        otts::core::log_info("srt_native", "C++ SRT play listening on 0.0.0.0:" + std::to_string(play_port_));
    }
    return true;
}

void SrtNativeServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (publish_socket_ != -1) {
        srt_close(publish_socket_);
        publish_socket_ = -1;
    }
    if (play_socket_ != -1) {
        srt_close(play_socket_);
        play_socket_ = -1;
    }
    if (publish_thread_.joinable()) {
        publish_thread_.join();
    }
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    srt_cleanup();
}

void SrtNativeServer::publish_loop() {
    while (running_.load()) {
        sockaddr_in client{};
        int len = sizeof(client);
        const auto socket = srt_accept(publish_socket_, reinterpret_cast<sockaddr*>(&client), &len);
        if (socket == SRT_INVALID_SOCK) {
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        std::thread(&SrtNativeServer::handle_publish_client, this, socket).detach();
    }
}

void SrtNativeServer::play_loop() {
    while (running_.load()) {
        sockaddr_in client{};
        int len = sizeof(client);
        const auto socket = srt_accept(play_socket_, reinterpret_cast<sockaddr*>(&client), &len);
        if (socket == SRT_INVALID_SOCK) {
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        std::thread(&SrtNativeServer::handle_play_client, this, socket).detach();
    }
}

void SrtNativeServer::handle_publish_client(int socket) {
    const auto stream_key = stream_key_from_socket(socket, publish_stream_key_);
    const auto session_key = "cpp-srt-publish:" + stream_key + ":" + std::to_string(socket);
    otts::core::log_info("srt_native", "publish client routed to key=" + stream_key);
    registry_.upsert_external_stream(stream_key, otts::media::StreamSource::Srt, "aac", "h264", "cpp-srt-native-publish", true);
    registry_.upsert_external_session(
        session_key,
        stream_key,
        otts::media::StreamSource::Srt,
        "publish",
        "cpp-srt-native-publish",
        "running",
        "srt://0.0.0.0:" + std::to_string(publish_port_),
        "srt://0.0.0.0:" + std::to_string(publish_port_) + "?mode=listener&transtype=live",
        "stream-registry",
        "srt/mpegts",
        "cxx-srt-mpegts-demux",
        "native-cxx",
        "h264/aac",
        0,
        now_epoch_ms(),
        0,
        0,
        0,
        "");

    H264TsExtractor video_demuxer;
    TsDemuxer pes_demuxer;
    std::array<std::uint8_t, 8192> buffer{};
    bool sent_sequence = false;
    bool sent_aac_sequence = false;
    NalSets known_sets;
    while (running_.load()) {
        const auto n = srt_recvmsg(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()));
        if (n <= 0) {
            otts::core::log_warn("srt_native", "publish recv ended key=" + stream_key + " err=" + srt_getlasterror_str());
            break;
        }
        auto audio_pes_packets = pes_demuxer.push(buffer.data(), static_cast<std::size_t>(n));
        for (const auto& pes : audio_pes_packets) {
            if ((pes.stream_id & 0xe0) != 0xc0 || pes.payload.empty()) {
                continue;
            }
            std::size_t cursor = 0;
            while (cursor + 7 <= pes.payload.size()) {
                const auto frame_length = adts_frame_length(pes.payload.data() + cursor, pes.payload.size() - cursor);
                if (frame_length < 7 || cursor + frame_length > pes.payload.size()) {
                    break;
                }
                const auto config = parse_adts_config({pes.payload.begin() + static_cast<std::ptrdiff_t>(cursor), pes.payload.begin() + static_cast<std::ptrdiff_t>(cursor + 7)});
                if (!sent_aac_sequence && config.valid) {
                    registry_.publish_external_media(
                        stream_key,
                        otts::media::StreamSource::Srt,
                        "cpp-srt-native-publish",
                        make_aac_sequence_header(pes.timestamp_ms, config));
                    sent_aac_sequence = true;
                }
                registry_.publish_external_media(
                    stream_key,
                    otts::media::StreamSource::Srt,
                    "cpp-srt-native-publish",
                    make_aac_raw_message(pes.timestamp_ms, pes.payload.data() + cursor + 7, frame_length - 7));
                cursor += frame_length;
            }
        }

        auto video_pes_packets = video_demuxer.push(buffer.data(), static_cast<std::size_t>(n));
        for (const auto& pes : video_pes_packets) {
            if ((pes.stream_id & 0xf0) != 0xe0 || pes.payload.empty()) {
                continue;
            }
            auto sets = extract_parameter_sets(pes.payload);
            if (!sets.sps.empty()) {
                known_sets.sps = std::move(sets.sps);
            }
            if (!sets.pps.empty()) {
                known_sets.pps = std::move(sets.pps);
            }
            if (!sent_sequence && !known_sets.sps.empty() && !known_sets.pps.empty()) {
                registry_.publish_external_media(
                    stream_key,
                    otts::media::StreamSource::Srt,
                    "cpp-srt-native-publish",
                    make_avc_sequence_header(pes.timestamp_ms, known_sets));
                sent_sequence = true;
            }
            auto media = make_avc_nalu_message(pes.timestamp_ms, pes.payload, has_idr(pes.payload));
            if (media.payload.size() > 5) {
                registry_.publish_external_media(stream_key, otts::media::StreamSource::Srt, "cpp-srt-native-publish", media);
            }
        }
    }
    for (const auto& pes : pes_demuxer.flush()) {
        if ((pes.stream_id & 0xe0) == 0xc0 && !pes.payload.empty()) {
            std::size_t cursor = 0;
            while (cursor + 7 <= pes.payload.size()) {
                const auto frame_length = adts_frame_length(pes.payload.data() + cursor, pes.payload.size() - cursor);
                if (frame_length < 7 || cursor + frame_length > pes.payload.size()) {
                    break;
                }
                registry_.publish_external_media(
                    stream_key,
                    otts::media::StreamSource::Srt,
                    "cpp-srt-native-publish",
                    make_aac_raw_message(pes.timestamp_ms, pes.payload.data() + cursor + 7, frame_length - 7));
                cursor += frame_length;
            }
        }
    }
    for (const auto& pes : video_demuxer.flush()) {
        if ((pes.stream_id & 0xf0) == 0xe0 && !pes.payload.empty()) {
            auto media = make_avc_nalu_message(pes.timestamp_ms, pes.payload, has_idr(pes.payload));
            if (media.payload.size() > 5) {
                registry_.publish_external_media(stream_key, otts::media::StreamSource::Srt, "cpp-srt-native-publish", media);
            }
        }
    }
    registry_.remove_external_session(session_key);
    registry_.remove_external_stream(stream_key, otts::media::StreamSource::Srt);
    srt_close(socket);
}

void SrtNativeServer::handle_play_client(int socket) {
    const auto stream_key = stream_key_from_socket(socket, publish_stream_key_);
    const auto session_key = "cpp-srt-play:" + stream_key + ":" + std::to_string(socket);
    otts::core::log_info("srt_native", "play client routed to key=" + stream_key);
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<otts::rtmp::MediaMessage> queue;
    bool alive = true;
    const auto callback_id = next_callback_id();
    registry_.upsert_external_session(
        session_key,
        stream_key,
        otts::media::StreamSource::Srt,
        "play",
        "cpp-srt-native-play",
        "running",
        "srt://0.0.0.0:" + std::to_string(play_port_),
        "srt://0.0.0.0:" + std::to_string(play_port_) + "?mode=listener&transtype=live",
        "stream-registry",
        "srt/mpegts",
        "cxx-srt-mpegts-mux",
        "native-cxx",
        "h264/aac",
        0,
        now_epoch_ms(),
        0,
        0,
        0,
        "");
    registry_.add_callback_subscriber(stream_key, callback_id, [&](const otts::rtmp::MediaMessage& message) {
        if (message.type_id != 9 && message.type_id != 8) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        // The registry snapshot can contain sequence headers plus a 512-packet
        // GOP. Keep the complete startup snapshot so late SRT clients never
        // lose SPS/PPS or the keyframe at the front of the GOP.
        if (queue.size() >= 1024) {
            queue.pop_front();
        }
        queue.push_back(message);
        cv.notify_one();
    });

    std::uint8_t pat_cc = 0;
    std::uint8_t pmt_cc = 0;
    std::uint8_t video_cc = 0;
    std::uint8_t audio_cc = 0;
    AacConfig aac_config;
    std::vector<std::uint8_t> avc_config;
    int sent_count = 0;
    while (running_.load()) {
        otts::rtmp::MediaMessage message;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(500), [&]() { return !queue.empty() || !alive; });
            if (queue.empty()) {
                continue;
            }
            message = std::move(queue.front());
            queue.pop_front();
        }
        if (message.type_id == 8 && message.payload.size() > 1 && message.payload[1] == 0) {
            const auto parsed = parse_flv_aac_config(message);
            if (parsed.valid) {
                aac_config = parsed;
            }
            continue;
        }
        if (message.type_id == 9 && message.payload.size() > 1 && message.payload[1] == 0) {
            auto parsed = flv_avc_config_to_annexb(message);
            if (!parsed.empty()) {
                avc_config = std::move(parsed);
            }
            continue;
        }
        std::vector<std::uint8_t> out;
        if ((sent_count++ % 30) == 0) {
            auto pat = make_pat(pat_cc++);
            auto pmt = make_pmt(pmt_cc++);
            out.insert(out.end(), pat.begin(), pat.end());
            out.insert(out.end(), pmt.begin(), pmt.end());
        }
        if (message.type_id == 9) {
            auto annexb = flv_video_to_annexb(message);
            if (annexb.empty()) {
                continue;
            }
            const bool keyframe = (message.payload[0] >> 4) == 1;
            if (keyframe && !avc_config.empty()) {
                std::vector<std::uint8_t> configured;
                configured.reserve(avc_config.size() + annexb.size());
                configured.insert(configured.end(), avc_config.begin(), avc_config.end());
                configured.insert(configured.end(), annexb.begin(), annexb.end());
                annexb = std::move(configured);
            }
            auto pes = make_pes_video(annexb, message.timestamp);
            auto ts = packetize_ts(kVideoPid, pes, video_cc);
            out.insert(out.end(), ts.begin(), ts.end());
        } else if (message.type_id == 8) {
            auto adts = make_adts_frame(message, aac_config);
            if (adts.empty()) {
                continue;
            }
            auto pes = make_pes_audio(adts, message.timestamp);
            auto ts = packetize_ts(kAudioPid, pes, audio_cc);
            out.insert(out.end(), ts.begin(), ts.end());
        }
        for (std::size_t offset = 0; offset < out.size();) {
            const auto chunk = std::min<std::size_t>(1316, out.size() - offset);
            const auto sent = srt_sendmsg(
                socket,
                reinterpret_cast<const char*>(out.data() + offset),
                static_cast<int>(chunk),
                -1,
                0);
            if (sent == SRT_ERROR) {
                alive = false;
                break;
            }
            offset += chunk;
        }
        if (!alive) {
            break;
        }
    }
    alive = false;
    registry_.remove_callback_subscriber(stream_key, callback_id);
    registry_.remove_external_session(session_key);
    srt_close(socket);
}

}  // namespace otts::srt
