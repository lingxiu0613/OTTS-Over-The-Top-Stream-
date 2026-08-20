#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace otts::webrtc {

struct TranscodedAudioFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t timestamp_ms{0};
};

class AudioTranscoder {
public:
    ~AudioTranscoder();

    AudioTranscoder(const AudioTranscoder&) = delete;
    AudioTranscoder& operator=(const AudioTranscoder&) = delete;

    static std::unique_ptr<AudioTranscoder> create_opus_to_aac(std::string& error);
    static std::unique_ptr<AudioTranscoder> create_aac_to_opus(
        const std::vector<std::uint8_t>& audio_specific_config,
        std::string& error);

    std::vector<TranscodedAudioFrame> transcode(
        const std::uint8_t* data,
        std::size_t size,
        std::uint32_t timestamp_ms);

    const std::vector<std::uint8_t>& output_codec_config() const;
    const std::string& last_error() const;

private:
    struct Impl;
    explicit AudioTranscoder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace otts::webrtc
