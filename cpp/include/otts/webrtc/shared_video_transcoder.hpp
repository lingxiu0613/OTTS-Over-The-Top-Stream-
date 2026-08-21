#pragma once

#include "otts/rtmp/stream_registry.hpp"
#include "otts/webrtc/video_transcoder.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace otts::webrtc {

class SharedVideoTranscodePipeline {
public:
    using Frame = std::shared_ptr<const TranscodedVideoFrame>;
    using SubscriberId = std::uint64_t;
    using FrameCallback = std::function<void(Frame)>;

    ~SharedVideoTranscodePipeline();
    SharedVideoTranscodePipeline(const SharedVideoTranscodePipeline&) = delete;
    SharedVideoTranscodePipeline& operator=(const SharedVideoTranscodePipeline&) = delete;

    SubscriberId subscribe(FrameCallback callback);
    void unsubscribe(SubscriberId subscriber_id);
    std::size_t subscriber_count() const;
    const std::string& stream_key() const;
    const VideoTranscodeSettings& settings() const;

private:
    struct Impl;
    explicit SharedVideoTranscodePipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend std::shared_ptr<SharedVideoTranscodePipeline> acquire_shared_hevc_to_avc_pipeline(
        otts::rtmp::StreamRegistry&,
        const std::string&,
        const VideoTranscodeSettings&,
        std::string&);
};

std::shared_ptr<SharedVideoTranscodePipeline> acquire_shared_hevc_to_avc_pipeline(
    otts::rtmp::StreamRegistry& registry,
    const std::string& stream_key,
    const VideoTranscodeSettings& settings,
    std::string& error);

}  // namespace otts::webrtc
