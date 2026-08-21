#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace otts::webrtc {

struct TranscodedVideoFrame {
    std::vector<std::uint8_t> annexb;
    std::uint32_t timestamp_ms{0};
    bool keyframe{false};
};

struct VideoTranscodeSettings {
    std::uint32_t bit_rate{2500000};
    std::uint32_t frame_rate{30};
    std::uint32_t gop_size{60};
    std::string encoder_name{"libx264"};
    std::string preset{"ultrafast"};
    std::string tune{"zerolatency"};

    std::string cache_key() const;
};

class VideoTranscoder {
public:
    ~VideoTranscoder();
    VideoTranscoder(const VideoTranscoder&) = delete;
    VideoTranscoder& operator=(const VideoTranscoder&) = delete;

    static std::unique_ptr<VideoTranscoder> create_hevc_to_avc(std::string& error);
    static std::unique_ptr<VideoTranscoder> create_hevc_to_avc(
        const VideoTranscodeSettings& settings,
        std::string& error);
    std::vector<TranscodedVideoFrame> transcode(
        const std::uint8_t* data,
        std::size_t size,
        std::uint32_t timestamp_ms);
    const std::string& last_error() const;

private:
    struct Impl;
    explicit VideoTranscoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace otts::webrtc
