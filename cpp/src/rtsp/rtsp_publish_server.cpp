#include "otts/rtsp/rtsp_publish_server.hpp"

#include "otts/auth/stream_auth.hpp"
#include "otts/codec/video_codec.hpp"
#include "otts/core/logger.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace otts::rtsp {
namespace {

std::uint64_t now_epoch_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::string query_value(const std::string& uri, const std::string& key) {
    const auto query = uri.find('?');
    if (query == std::string::npos) {
        return {};
    }
    std::size_t cursor = query + 1;
    while (cursor < uri.size()) {
        const auto next = uri.find('&', cursor);
        const auto part = uri.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        const auto equal = part.find('=');
        if (equal != std::string::npos && part.substr(0, equal) == key) {
            return part.substr(equal + 1);
        }
        if (next == std::string::npos) {
            break;
        }
        cursor = next + 1;
    }
    return {};
}

std::string stream_key_from_uri(std::string uri) {
    const auto query = uri.find('?');
    if (query != std::string::npos) {
        uri = uri.substr(0, query);
    }
    const auto scheme = uri.find("://");
    if (scheme != std::string::npos) {
        const auto slash = uri.find('/', scheme + 3);
        uri = slash == std::string::npos ? std::string{} : uri.substr(slash + 1);
    }
    while (!uri.empty() && uri.front() == '/') {
        uri.erase(uri.begin());
    }
    if (uri.size() > 4 && uri.substr(uri.size() - 4) == ".sdp") {
        uri.resize(uri.size() - 4);
    }
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (i + 1 < uri.size() && uri[i] == '_' && uri[i + 1] == '_') {
            out.push_back('/');
            ++i;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

struct RtspRequest {
    std::string method;
    std::string uri;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

std::optional<RtspRequest> parse_request(const std::string& raw) {
    const auto body_pos = raw.find("\r\n\r\n");
    const auto head = raw.substr(0, body_pos == std::string::npos ? raw.size() : body_pos);
    RtspRequest request;
    if (body_pos != std::string::npos) {
        request.body = raw.substr(body_pos + 4);
    }
    std::istringstream stream(head);
    std::string version;
    stream >> request.method >> request.uri >> version;
    if (request.method.empty()) {
        return std::nullopt;
    }
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return request;
}

std::size_t content_length_from_head(const std::string& head) {
    std::istringstream stream(head);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (lower(trim(line.substr(0, colon))) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoul(trim(line.substr(colon + 1))));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

std::string response_text(
    const std::string& status,
    const std::string& cseq,
    const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::ostringstream response;
    response << "RTSP/1.0 " << status << "\r\n";
    response << "CSeq: " << cseq << "\r\n";
    response << "Server: OTTS-CXX-RTSP-PUBLISH/0.1\r\n";
    for (const auto& [key, value] : headers) {
        response << key << ": " << value << "\r\n";
    }
    response << "\r\n";
    return response.str();
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_client_ports(const std::string& transport) {
    const auto key = lower(transport).find("client_port=");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const auto start = key + std::string("client_port=").size();
    const auto dash = transport.find('-', start);
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::make_pair(
            static_cast<std::uint16_t>(std::stoul(transport.substr(start, dash - start))),
            static_cast<std::uint16_t>(std::stoul(transport.substr(dash + 1))));
    } catch (...) {
        return std::nullopt;
    }
}

int bind_udp_port(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len) == 0) {
        port = ntohs(address.sin_port);
    }
    return fd;
}

struct NalSets {
    std::vector<std::uint8_t> vps;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

otts::media::CodecId parse_sdp_video_codec(const std::string& sdp) {
    const auto value = lower(sdp);
    return value.find(" h265/90000") != std::string::npos || value.find(" hevc/90000") != std::string::npos
        ? otts::media::CodecId::Hevc
        : otts::media::CodecId::Avc;
}

std::uint32_t parse_sdp_frame_interval_ms(const std::string& sdp) {
    std::istringstream lines(sdp);
    std::string line;
    while (std::getline(lines, line)) {
        line = lower(trim(std::move(line)));
        constexpr std::string_view frame_rate = "a=framerate:";
        constexpr std::string_view x_frame_rate = "a=x-framerate:";
        std::string value;
        if (line.rfind(frame_rate, 0) == 0) {
            value = line.substr(frame_rate.size());
        } else if (line.rfind(x_frame_rate, 0) == 0) {
            value = line.substr(x_frame_rate.size());
        } else {
            continue;
        }
        try {
            const auto fps = std::stod(value);
            if (fps >= 1.0 && fps <= 240.0) {
                return static_cast<std::uint32_t>(std::max(1.0, std::round(1000.0 / fps)));
            }
        } catch (...) {
        }
    }
    return 0;
}

struct AacSdpConfig {
    bool present{false};
    std::uint8_t payload_type{97};
    std::uint32_t sample_rate{44100};
    std::uint8_t channels{2};
    std::vector<std::uint8_t> audio_specific_config;
};

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    return -1;
}

std::vector<std::uint8_t> hex_decode(std::string value) {
    value = trim(std::move(value));
    if ((value.size() % 2) != 0) {
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const auto high = hex_digit(value[i]);
        const auto low = hex_digit(value[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

std::uint8_t aac_sample_rate_index(std::uint32_t sample_rate) {
    static constexpr std::array<std::uint32_t, 13> rates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350};
    for (std::uint8_t i = 0; i < rates.size(); ++i) {
        if (rates[i] == sample_rate) {
            return i;
        }
    }
    return 4;
}

std::vector<std::uint8_t> make_aac_audio_specific_config(std::uint32_t sample_rate, std::uint8_t channels) {
    constexpr std::uint8_t object_type = 2;
    const auto rate_index = aac_sample_rate_index(sample_rate);
    return {
        static_cast<std::uint8_t>((object_type << 3) | (rate_index >> 1)),
        static_cast<std::uint8_t>(((rate_index & 0x01) << 7) | ((channels & 0x0f) << 3))};
}

AacSdpConfig parse_sdp_aac(const std::string& sdp) {
    AacSdpConfig config;
    std::istringstream lines(sdp);
    std::string line;
    bool in_audio_section = false;
    while (std::getline(lines, line)) {
        line = trim(std::move(line));
        if (line.rfind("m=", 0) == 0) {
            in_audio_section = line.rfind("m=audio ", 0) == 0;
            if (in_audio_section) {
                std::istringstream media(line.substr(2));
                std::string kind;
                std::string port;
                std::string protocol;
                unsigned payload_type = 97;
                media >> kind >> port >> protocol >> payload_type;
                if (payload_type <= 127) {
                    config.payload_type = static_cast<std::uint8_t>(payload_type);
                }
            }
            continue;
        }
        if (!in_audio_section) {
            continue;
        }
        const auto rtpmap_prefix = std::string("a=rtpmap:") + std::to_string(config.payload_type) + " ";
        if (line.rfind(rtpmap_prefix, 0) == 0) {
            const auto encoding = line.substr(rtpmap_prefix.size());
            const auto encoding_lower = lower(encoding);
            if (encoding_lower.rfind("mpeg4-generic/", 0) != 0) {
                continue;
            }
            std::istringstream values(encoding.substr(std::string("MPEG4-GENERIC/").size()));
            std::string rate;
            std::string channels;
            std::getline(values, rate, '/');
            std::getline(values, channels, '/');
            try {
                config.sample_rate = static_cast<std::uint32_t>(std::stoul(rate));
                config.channels = channels.empty() ? 1 : static_cast<std::uint8_t>(std::stoul(channels));
                config.present = config.sample_rate > 0 && config.channels > 0;
            } catch (...) {
                config.present = false;
            }
            continue;
        }
        const auto fmtp_prefix = std::string("a=fmtp:") + std::to_string(config.payload_type) + " ";
        if (line.rfind(fmtp_prefix, 0) == 0) {
            auto parameters = line.substr(fmtp_prefix.size());
            const auto config_pos = lower(parameters).find("config=");
            if (config_pos != std::string::npos) {
                auto value = parameters.substr(config_pos + 7);
                const auto semicolon = value.find(';');
                if (semicolon != std::string::npos) {
                    value.resize(semicolon);
                }
                config.audio_specific_config = hex_decode(value);
            }
        }
    }
    if (config.present && config.audio_specific_config.empty()) {
        config.audio_specific_config = make_aac_audio_specific_config(config.sample_rate, config.channels);
    }
    return config;
}

std::vector<std::uint8_t> base64_decode(const std::string& input) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = -8;
    for (const unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto decoded = table[ch];
        if (decoded < 0) {
            return {};
        }
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

NalSets parse_sdp_sets(const std::string& sdp) {
    NalSets sets;
    std::istringstream lines(sdp);
    std::string line;
    while (std::getline(lines, line)) {
        const auto marker = std::string("sprop-parameter-sets=");
        const auto pos = line.find(marker);
        if (pos != std::string::npos) {
            auto value = line.substr(pos + marker.size());
            const auto semicolon = value.find(';');
            if (semicolon != std::string::npos) value.resize(semicolon);
            const auto comma = value.find(',');
            if (comma != std::string::npos) {
                sets.sps = base64_decode(value.substr(0, comma));
                sets.pps = base64_decode(value.substr(comma + 1));
            }
        }
        const auto parse_hevc_set = [&](const std::string& name, std::vector<std::uint8_t>& target) {
            const auto key = name + "=";
            const auto begin = line.find(key);
            if (begin == std::string::npos) return;
            auto value = line.substr(begin + key.size());
            const auto end = value.find(';');
            if (end != std::string::npos) value.resize(end);
            target = base64_decode(trim(std::move(value)));
        };
        parse_hevc_set("sprop-vps", sets.vps);
        parse_hevc_set("sprop-sps", sets.sps);
        parse_hevc_set("sprop-pps", sets.pps);
        }
    return sets;
}

otts::rtmp::MediaMessage make_aac_message(
    std::uint32_t timestamp_ms,
    std::uint8_t channels,
    std::uint8_t packet_type,
    const std::uint8_t* payload,
    std::size_t payload_size) {
    otts::rtmp::MediaMessage message;
    message.timestamp = timestamp_ms;
    message.type_id = 8;
    message.message_stream_id = 1;
    message.payload.reserve(payload_size + 2);
    message.payload.push_back(static_cast<std::uint8_t>(channels == 1 ? 0xae : 0xaf));
    message.payload.push_back(packet_type);
    message.payload.insert(message.payload.end(), payload, payload + payload_size);
    return message;
}

class AacRtpIngest {
public:
    AacRtpIngest(otts::rtmp::StreamRegistry& registry, std::string stream_key, AacSdpConfig config)
        : registry_(registry), stream_key_(std::move(stream_key)), config_(std::move(config)) {}

    void handle_packet(const std::uint8_t* packet, std::size_t size) {
        if (!config_.present || size < 12 || (packet[0] >> 6) != 2) {
            return;
        }
        const auto csrc_count = packet[0] & 0x0f;
        std::size_t offset = 12 + static_cast<std::size_t>(csrc_count) * 4;
        if (offset >= size) {
            return;
        }
        if ((packet[0] & 0x10) != 0) {
            if (offset + 4 > size) {
                return;
            }
            const auto extension_size = static_cast<std::size_t>((packet[offset + 2] << 8) | packet[offset + 3]) * 4;
            offset += 4 + extension_size;
            if (offset >= size) {
                return;
            }
        }
        const std::size_t padding = (packet[0] & 0x20) != 0 ? packet[size - 1] : 0;
        if (padding > size - offset) {
            return;
        }
        const auto payload_size = size - offset - padding;
        if (payload_size < 4) {
            return;
        }
        const auto rtp_timestamp =
            (static_cast<std::uint32_t>(packet[4]) << 24) |
            (static_cast<std::uint32_t>(packet[5]) << 16) |
            (static_cast<std::uint32_t>(packet[6]) << 8) |
            static_cast<std::uint32_t>(packet[7]);
        const auto* payload = packet + offset;
        const auto au_headers_bits = static_cast<std::size_t>((payload[0] << 8) | payload[1]);
        const auto au_headers_bytes = (au_headers_bits + 7) / 8;
        if (au_headers_bits < 16 || 2 + au_headers_bytes > payload_size) {
            return;
        }

        const auto au_count = au_headers_bits / 16;
        std::size_t data_offset = 2 + au_headers_bytes;
        for (std::size_t i = 0; i < au_count; ++i) {
            const auto header_offset = 2 + i * 2;
            if (header_offset + 2 > 2 + au_headers_bytes) {
                break;
            }
            const auto au_header = static_cast<std::uint16_t>((payload[header_offset] << 8) | payload[header_offset + 1]);
            const auto au_size = static_cast<std::size_t>(au_header >> 3);
            if (au_size == 0 || data_offset + au_size > payload_size) {
                break;
            }
            publish_sequence_header_once();
            const auto sample_timestamp = rtp_timestamp + static_cast<std::uint32_t>(i * 1024);
            const auto timestamp_ms = next_timestamp_ms(sample_timestamp);
            registry_.publish_external_media(
                stream_key_,
                otts::media::StreamSource::Rtsp,
                "cpp-rtsp-native-publish",
                make_aac_message(timestamp_ms, config_.channels, 1, payload + data_offset, au_size));
            data_offset += au_size;
        }
    }

private:
    void publish_sequence_header_once() {
        if (sent_sequence_ || config_.audio_specific_config.empty()) {
            return;
        }
        registry_.publish_external_media(
            stream_key_,
            otts::media::StreamSource::Rtsp,
            "cpp-rtsp-native-publish",
            make_aac_message(0, config_.channels, 0, config_.audio_specific_config.data(), config_.audio_specific_config.size()));
        sent_sequence_ = true;
    }

    std::uint32_t next_timestamp_ms(std::uint32_t rtp_timestamp) {
        if (!last_rtp_timestamp_.has_value()) {
            last_rtp_timestamp_ = rtp_timestamp;
            elapsed_samples_ = 0;
            timestamp_ms_ = 0;
            return 0;
        }
        const auto delta = rtp_timestamp - *last_rtp_timestamp_;
        const auto max_reasonable_delta = std::max<std::uint32_t>(config_.sample_rate * 2, 1024);
        const auto samples = delta > 0 && delta <= max_reasonable_delta ? delta : 1024;
        elapsed_samples_ += samples;
        *timestamp_ms_ = static_cast<std::uint32_t>((elapsed_samples_ * 1000) / config_.sample_rate);
        last_rtp_timestamp_ = rtp_timestamp;
        return *timestamp_ms_;
    }

    otts::rtmp::StreamRegistry& registry_;
    std::string stream_key_;
    AacSdpConfig config_;
    std::optional<std::uint32_t> last_rtp_timestamp_;
    std::optional<std::uint32_t> timestamp_ms_;
    std::uint64_t elapsed_samples_{0};
    bool sent_sequence_{false};
};

class VideoRtpIngest {
public:
    VideoRtpIngest(
        otts::rtmp::StreamRegistry& registry,
        std::string stream_key,
        NalSets sets,
        otts::media::CodecId codec,
        std::uint32_t frame_interval_ms)
        : registry_(registry),
          stream_key_(std::move(stream_key)),
          sets_(std::move(sets)),
          codec_(codec),
          frame_interval_ms_(frame_interval_ms == 0 ? 33 : frame_interval_ms),
          frame_interval_from_sdp_(frame_interval_ms != 0) {}

    void handle_packet(const std::uint8_t* packet, std::size_t size) {
        if (size < 12 || (packet[0] >> 6) != 2) {
            return;
        }
        const bool marker = (packet[1] & 0x80) != 0;
        const auto csrc_count = packet[0] & 0x0f;
        std::size_t offset = 12 + static_cast<std::size_t>(csrc_count) * 4;
        if (offset >= size) {
            return;
        }
        if ((packet[0] & 0x10) != 0) {
            if (offset + 4 > size) {
                return;
            }
            const auto ext_len = static_cast<std::size_t>((packet[offset + 2] << 8) | packet[offset + 3]) * 4;
            offset += 4 + ext_len;
            if (offset >= size) {
                return;
            }
        }
        const auto rtp_ts =
            (static_cast<std::uint32_t>(packet[4]) << 24) |
            (static_cast<std::uint32_t>(packet[5]) << 16) |
            (static_cast<std::uint32_t>(packet[6]) << 8) |
            static_cast<std::uint32_t>(packet[7]);
        const auto* payload = packet + offset;
        const auto payload_size = size - offset;
        if (payload_size == 0) {
            return;
        }

        const auto nal_type = codec_ == otts::media::CodecId::Hevc
            ? static_cast<std::uint8_t>((payload[0] >> 1) & 0x3f)
            : static_cast<std::uint8_t>(payload[0] & 0x1f);
        if ((codec_ == otts::media::CodecId::Avc && nal_type >= 1 && nal_type <= 23) ||
            (codec_ == otts::media::CodecId::Hevc && nal_type <= 47)) {
            append_start_code();
            current_au_.insert(current_au_.end(), payload, payload + payload_size);
        } else if ((codec_ == otts::media::CodecId::Avc && nal_type == 24) ||
                   (codec_ == otts::media::CodecId::Hevc && nal_type == 48)) {
            std::size_t cursor = codec_ == otts::media::CodecId::Hevc ? 2 : 1;
            while (cursor + 2 <= payload_size) {
                const auto nal_size = static_cast<std::size_t>((payload[cursor] << 8) | payload[cursor + 1]);
                cursor += 2;
                if (nal_size == 0 || cursor + nal_size > payload_size) {
                    break;
                }
                append_start_code();
                current_au_.insert(current_au_.end(), payload + cursor, payload + cursor + nal_size);
                cursor += nal_size;
            }
        } else if (codec_ == otts::media::CodecId::Avc && nal_type == 28 && payload_size >= 2) {
            const bool start = (payload[1] & 0x80) != 0;
            const auto reconstructed = static_cast<std::uint8_t>((payload[0] & 0xe0) | (payload[1] & 0x1f));
            if (start) {
                append_start_code();
                current_au_.push_back(reconstructed);
            }
            current_au_.insert(current_au_.end(), payload + 2, payload + payload_size);
        } else if (codec_ == otts::media::CodecId::Hevc && nal_type == 49 && payload_size >= 3) {
            const bool start = (payload[2] & 0x80) != 0;
            const auto original_type = static_cast<std::uint8_t>(payload[2] & 0x3f);
            if (start) {
                append_start_code();
                current_au_.push_back(static_cast<std::uint8_t>((payload[0] & 0x81) | (original_type << 1)));
                current_au_.push_back(payload[1]);
            }
            current_au_.insert(current_au_.end(), payload + 3, payload + payload_size);
        }

        if (marker && !current_au_.empty()) {
            const auto timing = next_timing(rtp_ts);
            publish(timing.dts_ms, timing.composition_time_ms);
            current_au_.clear();
        }
    }

private:
    struct VideoTiming {
        std::uint32_t dts_ms{0};
        std::int32_t composition_time_ms{0};
    };

    VideoTiming next_timing(std::uint32_t rtp_ts) {
        if (!last_rtp_ts_.has_value()) {
            last_rtp_ts_ = rtp_ts;
            return {};
        }

        const auto signed_delta_ticks = static_cast<std::int32_t>(rtp_ts - *last_rtp_ts_);
        const auto absolute_delta_ticks = static_cast<std::uint32_t>(
            signed_delta_ticks < 0 ? -static_cast<std::int64_t>(signed_delta_ticks) : signed_delta_ticks);
        if (!frame_interval_from_sdp_ && absolute_delta_ticks > 0) {
            timestamp_delta_gcd_ = timestamp_delta_gcd_ == 0
                ? absolute_delta_ticks : std::gcd(timestamp_delta_gcd_, absolute_delta_ticks);
            const auto candidate_ms = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(timestamp_delta_gcd_) * 1000 + 45000) / 90000);
            if (candidate_ms >= 4 && candidate_ms <= 100) {
                frame_interval_ms_ = candidate_ms;
            }
        }

        presentation_ticks_ += signed_delta_ticks;
        decode_timestamp_ms_ += frame_interval_ms_;
        last_rtp_ts_ = rtp_ts;
        const auto presentation_ms = (presentation_ticks_ * 1000) / 90000;
        const auto composition = presentation_ms - static_cast<std::int64_t>(decode_timestamp_ms_);
        return {
            decode_timestamp_ms_,
            static_cast<std::int32_t>(std::clamp<std::int64_t>(composition, -8388608, 8388607))};
    }

    void append_start_code() {
        current_au_.insert(current_au_.end(), {0x00, 0x00, 0x00, 0x01});
    }

    void publish(std::uint32_t dts_ms, std::int32_t composition_time_ms) {
        const auto parsed_sets = otts::codec::extract_parameter_sets(current_au_, codec_);
        if (!parsed_sets.vps.empty()) sets_.vps = parsed_sets.vps;
        if (!parsed_sets.sps.empty()) sets_.sps = parsed_sets.sps;
        if (!parsed_sets.pps.empty()) sets_.pps = parsed_sets.pps;
        otts::codec::ParameterSets codec_sets{sets_.vps, sets_.sps, sets_.pps};
        if (!sent_sequence_ && codec_sets.complete(codec_)) {
            otts::rtmp::MediaMessage config;
            config.timestamp = dts_ms;
            config.type_id = 9;
            config.payload = otts::codec::make_flv_video_config(codec_, codec_sets);
            registry_.publish_external_media(
                stream_key_,
                otts::media::StreamSource::Rtsp,
                "cpp-rtsp-native-publish",
                config);
            sent_sequence_ = true;
        }
        otts::rtmp::MediaMessage message;
        message.timestamp = dts_ms;
        message.type_id = 9;
        message.payload = otts::codec::make_flv_video_sample(
            codec_, current_au_, otts::codec::is_keyframe(current_au_, codec_), composition_time_ms);
        if (message.payload.size() > 5) {
            registry_.publish_external_media(stream_key_, otts::media::StreamSource::Rtsp, "cpp-rtsp-native-publish", message);
        }
    }

    otts::rtmp::StreamRegistry& registry_;
    std::string stream_key_;
    NalSets sets_;
    otts::media::CodecId codec_{otts::media::CodecId::Avc};
    std::optional<std::uint32_t> last_rtp_ts_;
    std::int64_t presentation_ticks_{0};
    std::uint32_t decode_timestamp_ms_{0};
    std::uint32_t timestamp_delta_gcd_{0};
    std::uint32_t frame_interval_ms_{33};
    bool frame_interval_from_sdp_{false};
    std::vector<std::uint8_t> current_au_;
    bool sent_sequence_{false};
};

}  // namespace

RtspPublishServer::RtspPublishServer(std::uint16_t port, otts::rtmp::StreamRegistry& registry)
    : port_(port), registry_(registry) {}

RtspPublishServer::~RtspPublishServer() {
    stop();
}

bool RtspPublishServer::start() {
    if (port_ == 0) {
        return true;
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        otts::core::log_error("rtsp_publish", std::string("bind failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 32) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    running_.store(true);
    std::thread(&RtspPublishServer::accept_loop, this).detach();
    otts::core::log_info("rtsp_publish", "C++ RTSP publish listening on 0.0.0.0:" + std::to_string(port_));
    return true;
}

void RtspPublishServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void RtspPublishServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0) {
            continue;
        }
        std::thread(&RtspPublishServer::handle_client, this, fd).detach();
    }
}

void RtspPublishServer::handle_client(int client_fd) {
    const auto session_id = std::to_string(next_session_id_.fetch_add(1));
    const auto session_key = "cpp-rtsp-publish:" + session_id;
    std::string buffer;
    std::string stream_key;
    NalSets sets;
    otts::media::CodecId video_codec = otts::media::CodecId::Avc;
    std::uint32_t video_frame_interval_ms = 0;
    AacSdpConfig aac_config;
    std::uint16_t video_rtp_port = 0;
    std::uint16_t video_rtcp_port = 0;
    int video_rtp_fd = -1;
    int video_rtcp_fd = -1;
    std::uint16_t audio_rtp_port = 0;
    std::uint16_t audio_rtcp_port = 0;
    int audio_rtp_fd = -1;
    int audio_rtcp_fd = -1;
    std::atomic<bool> receiving{false};
    std::thread video_receiver;
    std::thread audio_receiver;

    auto cleanup = [&]() {
        receiving.store(false);
        if (video_rtp_fd >= 0) {
            ::shutdown(video_rtp_fd, SHUT_RDWR);
            ::close(video_rtp_fd);
            video_rtp_fd = -1;
        }
        if (video_rtcp_fd >= 0) {
            ::shutdown(video_rtcp_fd, SHUT_RDWR);
            ::close(video_rtcp_fd);
            video_rtcp_fd = -1;
        }
        if (audio_rtp_fd >= 0) {
            ::shutdown(audio_rtp_fd, SHUT_RDWR);
            ::close(audio_rtp_fd);
            audio_rtp_fd = -1;
        }
        if (audio_rtcp_fd >= 0) {
            ::shutdown(audio_rtcp_fd, SHUT_RDWR);
            ::close(audio_rtcp_fd);
            audio_rtcp_fd = -1;
        }
        if (video_receiver.joinable()) {
            video_receiver.join();
        }
        if (audio_receiver.joinable()) {
            audio_receiver.join();
        }
        if (!stream_key.empty()) {
            registry_.remove_external_session(session_key);
            registry_.remove_external_stream(stream_key, otts::media::StreamSource::Rtsp);
        }
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
    };

    char recv_buf[4096];
    while (true) {
        const auto n = ::recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0) {
            cleanup();
            return;
        }
        buffer.append(recv_buf, static_cast<std::size_t>(n));
        while (true) {
            const auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                break;
            }
            const auto head = buffer.substr(0, end + 4);
            const auto content_length = content_length_from_head(head);
            if (buffer.size() < end + 4 + content_length) {
                break;
            }
            const auto raw = buffer.substr(0, end + 4 + content_length);
            buffer.erase(0, end + 4 + content_length);
            const auto request = parse_request(raw);
            if (!request.has_value()) {
                cleanup();
                return;
            }
            const auto cseq_it = request->headers.find("cseq");
            const auto cseq = cseq_it == request->headers.end() ? "1" : cseq_it->second;

            if (request->method == "OPTIONS") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Public", "OPTIONS, ANNOUNCE, SETUP, RECORD, GET_PARAMETER, SET_PARAMETER, TEARDOWN"}}));
                continue;
            }
            if (stream_key.empty()) {
                stream_key = stream_key_from_uri(request->uri);
            }
            if (!otts::auth::is_authorized(
                    "publish",
                    stream_key,
                    query_value(request->uri, "token"),
                    query_value(request->uri, "expires"),
                    query_value(request->uri, "sign"))) {
                send_all(client_fd, response_text("401 Unauthorized", cseq));
                cleanup();
                return;
            }
            if (request->method == "ANNOUNCE") {
                sets = parse_sdp_sets(request->body);
                video_codec = parse_sdp_video_codec(request->body);
                video_frame_interval_ms = parse_sdp_frame_interval_ms(request->body);
                aac_config = parse_sdp_aac(request->body);
                registry_.begin_external_publish(
                    stream_key, otts::media::StreamSource::Rtsp, "cpp-rtsp-native-publish");
                registry_.upsert_external_stream(
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    aac_config.present ? "aac" : "",
                    otts::media::to_string(video_codec),
                    "cpp-rtsp-native-publish",
                    true);
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "announced",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    video_codec == otts::media::CodecId::Hevc ? "cxx-rtsp-h265-rtp-demux" : "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    otts::media::to_string(video_codec) + (aac_config.present ? "/aac" : ""),
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                send_all(client_fd, response_text("200 OK", cseq));
                continue;
            }
            if (request->method == "SETUP") {
                const auto transport = request->headers.find("transport");
                if (transport == request->headers.end() || !parse_client_ports(transport->second).has_value()) {
                    send_all(client_fd, response_text("461 Unsupported Transport", cseq));
                    continue;
                }
                const auto uri_lower = lower(request->uri);
                const bool is_audio_track =
                    uri_lower.find("trackid=1") != std::string::npos ||
                    uri_lower.find("streamid=1") != std::string::npos ||
                    uri_lower.find("stream=1") != std::string::npos ||
                    uri_lower.find("/audio") != std::string::npos;
                std::uint16_t* target_rtp_port = is_audio_track ? &audio_rtp_port : &video_rtp_port;
                std::uint16_t* target_rtcp_port = is_audio_track ? &audio_rtcp_port : &video_rtcp_port;
                int* target_rtp_fd = is_audio_track ? &audio_rtp_fd : &video_rtp_fd;
                int* target_rtcp_fd = is_audio_track ? &audio_rtcp_fd : &video_rtcp_fd;
                if (*target_rtp_fd >= 0) {
                    ::close(*target_rtp_fd);
                    *target_rtp_fd = -1;
                }
                if (*target_rtcp_fd >= 0) {
                    ::close(*target_rtcp_fd);
                    *target_rtcp_fd = -1;
                }
                for (int attempt = 0; attempt < 32; ++attempt) {
                    *target_rtp_port = 0;
                    *target_rtp_fd = bind_udp_port(*target_rtp_port);
                    if (*target_rtp_fd < 0 || (*target_rtp_port % 2) != 0) {
                        if (*target_rtp_fd >= 0) {
                            ::close(*target_rtp_fd);
                            *target_rtp_fd = -1;
                        }
                        continue;
                    }
                    *target_rtcp_port = static_cast<std::uint16_t>(*target_rtp_port + 1);
                    *target_rtcp_fd = bind_udp_port(*target_rtcp_port);
                    if (*target_rtcp_fd >= 0) {
                        break;
                    }
                    ::close(*target_rtp_fd);
                    *target_rtp_fd = -1;
                }
                if (*target_rtp_fd < 0 || *target_rtcp_fd < 0) {
                    send_all(client_fd, response_text("500 Internal Server Error", cseq));
                    continue;
                }
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "setup",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    aac_config.present ? "cxx-rtsp-h264-aac-rtp-demux" : "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    aac_config.present ? "h264/aac" : "h264",
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                std::ostringstream transport_response;
                const auto ports = parse_client_ports(transport->second).value();
                transport_response << "RTP/AVP/UDP;unicast;client_port=" << ports.first << "-" << ports.second
                                   << ";server_port=" << *target_rtp_port << "-" << *target_rtcp_port;
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}, {"Transport", transport_response.str()}}));
                continue;
            }
            if (request->method == "RECORD") {
                if (video_rtp_fd < 0) {
                    send_all(client_fd, response_text("455 Method Not Valid in This State", cseq));
                    continue;
                }
                receiving.store(true);
                video_receiver = std::thread([&, local_rtp_fd = video_rtp_fd]() {
                    VideoRtpIngest ingest(
                        registry_, stream_key, sets, video_codec, video_frame_interval_ms);
                    std::array<std::uint8_t, 2048> packet{};
                    while (receiving.load()) {
                        const auto received = ::recv(local_rtp_fd, packet.data(), packet.size(), 0);
                        if (received <= 0) {
                            break;
                        }
                        ingest.handle_packet(packet.data(), static_cast<std::size_t>(received));
                    }
                });
                if (audio_rtp_fd >= 0 && aac_config.present) {
                    audio_receiver = std::thread([&, local_audio_rtp_fd = audio_rtp_fd]() {
                        AacRtpIngest ingest(registry_, stream_key, aac_config);
                        std::array<std::uint8_t, 2048> packet{};
                        while (receiving.load()) {
                            const auto received = ::recv(local_audio_rtp_fd, packet.data(), packet.size(), 0);
                            if (received <= 0) {
                                break;
                            }
                            ingest.handle_packet(packet.data(), static_cast<std::size_t>(received));
                        }
                    });
                }
                registry_.upsert_external_session(
                    session_key,
                    stream_key,
                    otts::media::StreamSource::Rtsp,
                    "publish",
                    "cpp-rtsp-native-publish",
                    "recording",
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "rtsp://0.0.0.0:" + std::to_string(port_),
                    "stream-registry",
                    "rtsp/rtp/udp",
                    aac_config.present ? "cxx-rtsp-h264-aac-rtp-demux" : "cxx-rtsp-h264-rtp-demux",
                    "native-cxx",
                    aac_config.present ? "h264/aac" : "h264",
                    0,
                    now_epoch_ms(),
                    0,
                    0,
                    0,
                    "");
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request->method == "GET_PARAMETER" || request->method == "SET_PARAMETER") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                continue;
            }

            if (request->method == "TEARDOWN") {
                send_all(client_fd, response_text("200 OK", cseq, {{"Session", session_id}}));
                cleanup();
                return;
            }
            send_all(client_fd, response_text("405 Method Not Allowed", cseq));
        }
    }
}

}  // namespace otts::rtsp
