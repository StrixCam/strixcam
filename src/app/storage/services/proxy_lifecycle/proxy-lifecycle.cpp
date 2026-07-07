#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace sst::storage {

namespace {

auto SystemEpochMs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

ProxyLifecycle::ProxyLifecycle(IRawCaptureSink& sink, Clock now_ms)
    : sink_(sink), now_ms_(std::move(now_ms)) {}

auto ProxyLifecycle::StartLocked(const std::string& group_id,
                                 const std::filesystem::path& output_dir) -> void {
    proxy_running_ = sink_.Start(group_id, output_dir);
    if (proxy_running_) {
        active_group_ = group_id;
    } else {
        // Non-fatal by contract: the match record / stream must proceed without
        // training footage rather than fail on it.
        spdlog::warn("ProxyLifecycle: training proxy failed to start for group {} (continuing)",
                     group_id);
        active_group_.clear();
    }
}

auto ProxyLifecycle::StopIfIdleLocked() -> void {
    if (record_active_ || stream_active_ || !proxy_running_) {
        return;
    }
    sink_.Stop();
    proxy_running_ = false;
    active_group_.clear();
}

auto ProxyLifecycle::OnRecordingStart(const std::string& capture_group_id,
                                      const std::filesystem::path& output_dir) -> void {
    std::lock_guard lock(mtx_);
    record_active_ = true;
    if (capture_group_id.empty()) {
        return;  // hold only — no proxy identity from this record
    }
    if (proxy_running_ && active_group_ == capture_group_id) {
        return;
    }
    if (proxy_running_) {
        // Rebind: the app-minted match id is the download-grouping contract, so
        // an interim (stream-minted) or stale-match proxy restarts under it —
        // the momentary gap is preferable to mis-grouped training footage.
        spdlog::info("ProxyLifecycle: rebinding proxy {} -> {}", active_group_, capture_group_id);
        sink_.Stop();
        proxy_running_ = false;
    }
    StartLocked(capture_group_id, output_dir);
}

auto ProxyLifecycle::OnRecordingStop() -> void {
    std::lock_guard lock(mtx_);
    record_active_ = false;
    StopIfIdleLocked();
}

auto ProxyLifecycle::OnStreamingStart() -> void {
    std::lock_guard lock(mtx_);
    stream_active_ = true;
    if (proxy_running_) {
        return;
    }
    // Stream-only: no app-minted id and no per-match dir exist, so mint an
    // interim id (a record START later rebinds to the real match id).
    const std::uint64_t stamp = now_ms_ ? now_ms_() : SystemEpochMs();
    StartLocked("stream-" + std::to_string(stamp), {});
}

auto ProxyLifecycle::OnStreamingStop() -> void {
    std::lock_guard lock(mtx_);
    stream_active_ = false;
    StopIfIdleLocked();
}

auto ProxyLifecycle::ForceStop() -> void {
    std::lock_guard lock(mtx_);
    record_active_ = false;
    stream_active_ = false;
    // Unconditional sink stop (idempotent, a no-op when idle): session end must
    // also close a standalone raw capture the lifecycle never started —
    // preserving the pre-ref-count SessionCleanup behaviour.
    sink_.Stop();
    proxy_running_ = false;
    active_group_.clear();
}

auto ProxyLifecycle::IsProxyRunning() const -> bool {
    std::lock_guard lock(mtx_);
    return proxy_running_;
}

}  // namespace sst::storage
