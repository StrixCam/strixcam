#include "app/streaming/services/streaming_service/streaming-service.hpp"

#include <spdlog/spdlog.h>

#include <utility>

#include "domain/streaming/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::streaming {

StreamingService::StreamingService(std::unique_ptr<IAppStreamServer> app_stream_server,
                                   PlatformStreamerFactory platform_streamer_factory)
    : app_stream_server_(std::move(app_stream_server)),
      platform_streamer_factory_(std::move(platform_streamer_factory)) {}

StreamingService::~StreamingService() {
    std::lock_guard lock(mtx_);
    if (app_stream_server_ && app_stream_server_->IsRunning()) {
        app_stream_server_->Stop();
    }
    for (auto& [id, streamer] : platform_streams_) {
        if (streamer && streamer->IsRunning()) {
            streamer->Stop();
        }
    }
    platform_streams_.clear();
    active_names_.clear();
}

auto StreamingService::StartAppStream(const AppStreamConfig& config) -> bool {
    std::lock_guard lock(mtx_);
    if (app_stream_server_->IsRunning()) {
        spdlog::info("StreamingService::StartAppStream: already running");
        return true;
    }
    const bool started = app_stream_server_->Start(config);
    if (started) {
        spdlog::info("StreamingService::StartAppStream OK: {}", config);
    } else {
        spdlog::error("StreamingService::StartAppStream failed: {}", config);
    }
    return started;
}

auto StreamingService::StopAppStream() -> bool {
    std::lock_guard lock(mtx_);
    if (!app_stream_server_->IsRunning()) {
        return false;
    }
    app_stream_server_->Stop();
    spdlog::info("StreamingService::StopAppStream OK");
    return true;
}

auto StreamingService::IsAppStreamRunning() const -> bool {
    std::lock_guard lock(mtx_);
    return app_stream_server_->IsRunning();
}

auto StreamingService::StartPlatformStream(const PlatformStreamConfig& config) -> bool {
    if (config.stream_id <= 0) {
        spdlog::warn("StreamingService::StartPlatformStream rejected: invalid stream_id={}",
                     config.stream_id);
        return false;
    }

    std::lock_guard lock(mtx_);
    if (platform_streams_.contains(config.stream_id)) {
        spdlog::warn(
            "StreamingService::StartPlatformStream rejected: stream_id={} "
            "already active",
            config.stream_id);
        return false;
    }

    auto streamer = platform_streamer_factory_();
    if (!streamer) {
        spdlog::error("StreamingService::StartPlatformStream: factory returned null");
        return false;
    }
    if (!streamer->Start(config)) {
        spdlog::error("StreamingService::StartPlatformStream: streamer failed to start: {}",
                      config);
        return false;
    }
    spdlog::info("StreamingService::StartPlatformStream OK: {}", config);
    active_names_[config.stream_id] = config.name;
    platform_streams_.emplace(config.stream_id, std::move(streamer));
    return true;
}

auto StreamingService::StopPlatformStream(std::int64_t stream_id) -> bool {
    std::unique_ptr<IPlatformStreamer> taken;
    {
        std::lock_guard lock(mtx_);
        auto entry = platform_streams_.find(stream_id);
        if (entry == platform_streams_.end()) {
            return false;
        }
        taken = std::move(entry->second);
        platform_streams_.erase(entry);
        active_names_.erase(stream_id);
    }
    if (taken) {
        taken->Stop();
    }
    spdlog::info("StreamingService::StopPlatformStream OK: id={}", stream_id);
    return true;
}

auto StreamingService::ListActivePlatformStreams() const -> std::vector<ActivePlatformStream> {
    std::lock_guard lock(mtx_);
    std::vector<ActivePlatformStream> out;
    out.reserve(active_names_.size());
    for (const auto& [id, name] : active_names_) {
        out.push_back(ActivePlatformStream{.stream_id = id, .name = name});
    }
    return out;
}

auto StreamingService::Push(const sst::capture::Frame& frame) -> void {
    std::lock_guard lock(mtx_);
    if (app_stream_server_ && app_stream_server_->IsRunning()) {
        app_stream_server_->Push(frame);
    }
    for (auto& [id, streamer] : platform_streams_) {
        if (streamer && streamer->IsRunning()) {
            streamer->Push(frame);
        }
    }
}

}  // namespace sst::streaming
