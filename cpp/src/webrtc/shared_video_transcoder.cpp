#include "otts/webrtc/shared_video_transcoder.hpp"

#include "otts/codec/video_codec.hpp"
#include "otts/core/logger.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace otts::webrtc {
namespace {

constexpr std::size_t kMaxSourceQueue = 1024;
constexpr std::size_t kMaxCachedFrames = 512;

std::atomic<otts::rtmp::StreamRegistry::CallbackId> next_registry_callback_id{0x4000000000000000ULL};

struct PoolKey {
    otts::rtmp::StreamRegistry* registry{nullptr};
    std::string stream_key;
    std::string settings_key;

    bool operator==(const PoolKey& other) const {
        return registry == other.registry && stream_key == other.stream_key && settings_key == other.settings_key;
    }
};

struct PoolKeyHash {
    std::size_t operator()(const PoolKey& key) const {
        auto value = std::hash<void*>{}(key.registry);
        value ^= std::hash<std::string>{}(key.stream_key) + 0x9e3779b9 + (value << 6) + (value >> 2);
        value ^= std::hash<std::string>{}(key.settings_key) + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }
};

std::mutex pool_mutex;
std::unordered_map<PoolKey, std::weak_ptr<SharedVideoTranscodePipeline>, PoolKeyHash> pipeline_pool;

}  // namespace

struct SharedVideoTranscodePipeline::Impl {
    otts::rtmp::StreamRegistry& registry;
    std::string stream_key;
    VideoTranscodeSettings settings;
    otts::rtmp::StreamRegistry::CallbackId registry_callback_id{0};

    mutable std::mutex subscriber_mutex;
    std::unordered_map<SubscriberId, FrameCallback> subscribers;
    SubscriberId next_subscriber_id{1};
    std::deque<Frame> gop_cache;
    bool cache_has_keyframe{false};

    std::mutex source_mutex;
    std::condition_variable source_cv;
    std::deque<otts::rtmp::MediaMessage> source_queue;
    std::optional<otts::rtmp::MediaMessage> latest_config;
    bool source_waiting_for_keyframe{false};
    bool reset_requested{false};
    bool stop{false};
    std::thread worker;

    Impl(
        otts::rtmp::StreamRegistry& registry_value,
        std::string stream_key_value,
        VideoTranscodeSettings settings_value)
        : registry(registry_value),
          stream_key(std::move(stream_key_value)),
          settings(std::move(settings_value)) {}

    void start(const std::shared_ptr<SharedVideoTranscodePipeline>& owner) {
        worker = std::thread([this]() { worker_loop(); });
        registry_callback_id = next_registry_callback_id.fetch_add(1, std::memory_order_relaxed);
        registry.add_callback_subscriber(
            stream_key,
            registry_callback_id,
            [weak = std::weak_ptr<SharedVideoTranscodePipeline>(owner)](const otts::rtmp::MediaMessage& message) {
                if (auto pipeline = weak.lock()) {
                    pipeline->impl_->enqueue_source(message);
                }
            });
    }

    void shutdown() {
        if (registry_callback_id != 0) {
            registry.remove_callback_subscriber(stream_key, registry_callback_id);
            registry_callback_id = 0;
        }
        {
            std::lock_guard<std::mutex> lock(source_mutex);
            stop = true;
            source_queue.clear();
        }
        source_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void enqueue_source(const otts::rtmp::MediaMessage& message) {
        if (message.type_id != 9) return;
        const auto packet = otts::codec::parse_flv_video_packet(message.payload);
        if (!packet || packet->codec != otts::media::CodecId::Hevc) return;

        std::lock_guard<std::mutex> lock(source_mutex);
        if (stop) return;
        if (packet->sequence_header) {
            latest_config = message;
        }
        if (source_queue.size() >= kMaxSourceQueue) {
            source_queue.clear();
            source_waiting_for_keyframe = true;
            reset_requested = true;
            otts::core::log_warn(
                "webrtc_transcode",
                "shared source queue overflow; waiting for HEVC keyframe key=" + stream_key);
        }
        if (source_waiting_for_keyframe) {
            if (packet->sequence_header) return;
            if (!packet->coded_frames || !packet->keyframe) return;
            if (latest_config) source_queue.push_back(*latest_config);
            source_waiting_for_keyframe = false;
        }
        source_queue.push_back(message);
        source_cv.notify_one();
    }

    void publish_frame(TranscodedVideoFrame frame) {
        auto shared_frame = std::make_shared<const TranscodedVideoFrame>(std::move(frame));
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        if (shared_frame->keyframe) {
            gop_cache.clear();
            cache_has_keyframe = true;
        }
        if (cache_has_keyframe) {
            gop_cache.push_back(shared_frame);
            while (gop_cache.size() > kMaxCachedFrames) gop_cache.pop_front();
        }
        for (const auto& [id, callback] : subscribers) {
            (void)id;
            try {
                callback(shared_frame);
            } catch (const std::exception& exc) {
                otts::core::log_warn(
                    "webrtc_transcode",
                    "shared subscriber callback failed key=" + stream_key + " error=" + exc.what());
            }
        }
    }

    void reset_cache() {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        gop_cache.clear();
        cache_has_keyframe = false;
    }

    void worker_loop() {
        std::unique_ptr<VideoTranscoder> transcoder;
        std::vector<std::uint8_t> video_config;
        bool keyframe_seen = false;
        bool timestamp_seen = false;
        std::uint32_t last_timestamp = 0;
        otts::codec::ParameterSets avc_parameter_sets;

        while (true) {
            otts::rtmp::MediaMessage message;
            bool should_reset = false;
            {
                std::unique_lock<std::mutex> lock(source_mutex);
                source_cv.wait(lock, [&]() { return stop || !source_queue.empty(); });
                if (stop && source_queue.empty()) break;
                message = std::move(source_queue.front());
                source_queue.pop_front();
                should_reset = std::exchange(reset_requested, false);
            }

            const auto packet = otts::codec::parse_flv_video_packet(message.payload);
            if (!packet || packet->codec != otts::media::CodecId::Hevc) continue;
            if (timestamp_seen && message.timestamp < last_timestamp &&
                last_timestamp - message.timestamp > 1000) should_reset = true;
            timestamp_seen = true;
            last_timestamp = message.timestamp;
            if (should_reset) {
                transcoder.reset();
                keyframe_seen = false;
                avc_parameter_sets = {};
                reset_cache();
            }
            if (packet->sequence_header) {
                auto config = otts::codec::flv_video_config_to_annexb(message.payload);
                if (!config.empty() && !video_config.empty() && config != video_config) {
                    transcoder.reset();
                    keyframe_seen = false;
                    avc_parameter_sets = {};
                    reset_cache();
                }
                if (!config.empty()) video_config = std::move(config);
                continue;
            }
            if (!packet->coded_frames) continue;
            auto sample = otts::codec::flv_video_sample_to_annexb(message.payload);
            const bool keyframe = packet->keyframe ||
                otts::codec::is_keyframe(sample, otts::media::CodecId::Hevc);
            if (!keyframe && !keyframe_seen) continue;
            if (keyframe) {
                if (!video_config.empty()) {
                    auto configured = video_config;
                    configured.insert(configured.end(), sample.begin(), sample.end());
                    sample = std::move(configured);
                }
                keyframe_seen = true;
            }
            if (!transcoder) {
                std::string error;
                transcoder = VideoTranscoder::create_hevc_to_avc(settings, error);
                if (!transcoder) {
                    otts::core::log_warn(
                        "webrtc_transcode",
                        "shared HEVC-to-AVC init failed key=" + stream_key + " error=" + error);
                    keyframe_seen = false;
                    continue;
                }
                otts::core::log_info(
                    "webrtc_transcode",
                    "shared HEVC decoder + H.264 encoder ready key=" + stream_key +
                        " profile=" + settings.cache_key());
            }
            for (auto& frame : transcoder->transcode(sample.data(), sample.size(), message.timestamp)) {
                // AV_PKT_FLAG_KEY can also describe an intra picture that is
                // not an IDR access point. Only an actual H.264 IDR may start
                // the cached GOP used to bootstrap a new decoder.
                frame.keyframe = otts::codec::is_keyframe(
                    frame.annexb, otts::media::CodecId::Avc);
                const auto encoded_sets = otts::codec::extract_parameter_sets(
                    frame.annexb, otts::media::CodecId::Avc);
                if (!encoded_sets.sps.empty()) avc_parameter_sets.sps = encoded_sets.sps;
                if (!encoded_sets.pps.empty()) avc_parameter_sets.pps = encoded_sets.pps;
                // Some libx264 builds only emit SPS/PPS with the first IDR even
                // when repeat-headers is requested. Every cached GOP must be
                // independently decodable by a late WHEP subscriber.
                if (frame.keyframe && avc_parameter_sets.complete(otts::media::CodecId::Avc)) {
                    auto configured = avc_parameter_sets.annexb();
                    configured.insert(configured.end(), frame.annexb.begin(), frame.annexb.end());
                    frame.annexb = std::move(configured);
                }
                publish_frame(std::move(frame));
            }
        }
    }
};

SharedVideoTranscodePipeline::SharedVideoTranscodePipeline(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SharedVideoTranscodePipeline::~SharedVideoTranscodePipeline() {
    impl_->shutdown();
    otts::core::log_info("webrtc_transcode", "shared pipeline stopped key=" + impl_->stream_key);
}

SharedVideoTranscodePipeline::SubscriberId SharedVideoTranscodePipeline::subscribe(FrameCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(impl_->subscriber_mutex);
    const auto id = impl_->next_subscriber_id++;
    impl_->subscribers.emplace(id, callback);
    for (const auto& frame : impl_->gop_cache) callback(frame);
    otts::core::log_info(
        "webrtc_transcode",
        "shared subscriber added key=" + impl_->stream_key +
            " count=" + std::to_string(impl_->subscribers.size()));
    return id;
}

void SharedVideoTranscodePipeline::unsubscribe(SubscriberId subscriber_id) {
    std::lock_guard<std::mutex> lock(impl_->subscriber_mutex);
    impl_->subscribers.erase(subscriber_id);
    otts::core::log_info(
        "webrtc_transcode",
        "shared subscriber removed key=" + impl_->stream_key +
            " count=" + std::to_string(impl_->subscribers.size()));
}

std::size_t SharedVideoTranscodePipeline::subscriber_count() const {
    std::lock_guard<std::mutex> lock(impl_->subscriber_mutex);
    return impl_->subscribers.size();
}

const std::string& SharedVideoTranscodePipeline::stream_key() const { return impl_->stream_key; }
const VideoTranscodeSettings& SharedVideoTranscodePipeline::settings() const { return impl_->settings; }

std::shared_ptr<SharedVideoTranscodePipeline> acquire_shared_hevc_to_avc_pipeline(
    otts::rtmp::StreamRegistry& registry,
    const std::string& stream_key,
    const VideoTranscodeSettings& settings,
    std::string& error) {
    if (stream_key.empty()) {
        error = "empty stream key";
        return nullptr;
    }
    const PoolKey key{&registry, stream_key, settings.cache_key()};
    std::lock_guard<std::mutex> lock(pool_mutex);
    for (auto it = pipeline_pool.begin(); it != pipeline_pool.end();) {
        if (it->second.expired()) it = pipeline_pool.erase(it);
        else ++it;
    }
    if (const auto existing = pipeline_pool.find(key); existing != pipeline_pool.end()) {
        if (auto pipeline = existing->second.lock()) return pipeline;
    }
    auto pipeline = std::shared_ptr<SharedVideoTranscodePipeline>(
        new SharedVideoTranscodePipeline(std::make_unique<SharedVideoTranscodePipeline::Impl>(
            registry, stream_key, settings)));
    pipeline->impl_->start(pipeline);
    pipeline_pool[key] = pipeline;
    otts::core::log_info(
        "webrtc_transcode",
        "shared pipeline created key=" + stream_key + " profile=" + settings.cache_key());
    return pipeline;
}

}  // namespace otts::webrtc
