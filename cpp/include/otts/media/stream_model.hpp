#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace otts::media {

enum class MediaKind {
    Metadata,
    Audio,
    Video,
    Data
};

enum class CodecId {
    Unknown,
    Aac,
    Avc,
    Hevc,
    Opus
};

enum class StreamSource {
    Unknown,
    Rtmp,
    Whip,
    Rtsp,
    Srt
};

struct MediaPacket {
    MediaKind kind{MediaKind::Data};
    CodecId codec{CodecId::Unknown};
    std::uint32_t timestamp_ms{0};
    std::uint32_t message_stream_id{1};
    bool is_sequence_header{false};
    bool is_keyframe{false};
    std::vector<std::uint8_t> payload;
};

struct TrackState {
    MediaKind kind{MediaKind::Data};
    CodecId codec{CodecId::Unknown};
    bool present{false};
    bool has_sequence_header{false};
    std::uint32_t last_timestamp_ms{0};
    std::uint64_t packets{0};
    std::uint64_t bytes{0};
};

class GopCache {
public:
    void add(const MediaPacket& packet) {
        if (packet.kind == MediaKind::Video && packet.is_keyframe) {
            packets_.clear();
        }
        packets_.push_back(packet);
        if (packets_.size() > max_packets_) {
            packets_.erase(packets_.begin());
        }
    }

    [[nodiscard]] const std::vector<MediaPacket>& packets() const {
        return packets_;
    }

    [[nodiscard]] std::size_t size() const {
        return packets_.size();
    }

    void clear() {
        packets_.clear();
    }

private:
    std::size_t max_packets_{512};
    std::vector<MediaPacket> packets_;
};

inline std::string to_string(CodecId codec) {
    switch (codec) {
        case CodecId::Aac:
            return "aac";
        case CodecId::Avc:
            return "h264";
        case CodecId::Hevc:
            return "h265";
        case CodecId::Opus:
            return "opus";
        case CodecId::Unknown:
        default:
            return "unknown";
    }
}

inline std::string to_string(StreamSource source) {
    switch (source) {
        case StreamSource::Rtmp:
            return "rtmp";
        case StreamSource::Whip:
            return "whip";
        case StreamSource::Rtsp:
            return "rtsp";
        case StreamSource::Srt:
            return "srt";
        case StreamSource::Unknown:
        default:
            return "unknown";
    }
}

}  // namespace otts::media
