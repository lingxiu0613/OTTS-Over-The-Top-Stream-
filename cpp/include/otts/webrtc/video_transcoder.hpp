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

class VideoTranscoder {
public:
    ~VideoTranscoder();
    VideoTranscoder(const VideoTranscoder&) = delete;
    VideoTranscoder& operator=(const VideoTranscoder&) = delete;

    static std::unique_ptr<VideoTranscoder> create_hevc_to_avc(std::string& error);
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
