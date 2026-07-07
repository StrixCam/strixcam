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

ProxyLifecycle::ProxyLifecycle(IProxySink& sink, SessionInfoProvider session_info, Clock now_ms)
    : sink_(sink), session_info_(std::move(session_info)), now_ms_(std::move(now_ms)) {}

auto ProxyLifecycle::CurrentSessionInfo() const -> std::optional<ProxySessionInfo> {
    if (!session_info_) {
        return std::nullopt;
    }
    auto info = session_info_();
    if (!info || info->match_uuid.empty()) {
        return std::nullopt;
    }
    return info;
}

auto ProxyLifecycle::StartLocked(const std::string& match_uuid,
                                 const std::filesystem::path& output_dir) -> void {
    proxy_running_ = sink_.Start(match_uuid, output_dir);
    if (proxy_running_) {
        active_match_ = match_uuid;
    } else {
        // Non-fatal by contract: the match record / stream must proceed without
        // development footage rather than fail on it.
        spdlog::warn("ProxyLifecycle: internal proxy failed to start for {} (continuing)",
                     match_uuid);
        active_match_.clear();
    }
}

auto ProxyLifecycle::StopIfIdleLocked() -> void {
    if (record_active_ || stream_active_ || !proxy_running_) {
        return;
    }
    sink_.Stop();
    proxy_running_ = false;
    active_match_.clear();
}

auto ProxyLifecycle::OnRecordingStart() -> void {
    std::lock_guard lock(mtx_);
    record_active_ = true;
    const auto info = CurrentSessionInfo();
    if (!info) {
        return;  // hold only — no session identity to pair a proxy with
    }
    if (proxy_running_ && active_match_ == info->match_uuid) {
        return;
    }
    if (proxy_running_) {
        // Rebind: the session's match_uuid is the on-device pairing contract, so
        // an interim (stream-minted) or stale-match proxy restarts under it —
        // the momentary gap is preferable to mis-paired development footage.
        spdlog::info("ProxyLifecycle: rebinding proxy {} -> {}", active_match_, info->match_uuid);
        sink_.Stop();
        proxy_running_ = false;
    }
    StartLocked(info->match_uuid, info->output_dir);
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
    // Stream-only sessions still carry the pushed match identity when one
    // exists; with no session config there is no match id and no per-match dir,
    // so mint an interim id at the sink root (a record start later rebinds to
    // the real match id).
    if (const auto info = CurrentSessionInfo()) {
        StartLocked(info->match_uuid, info->output_dir);
        return;
    }
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
    // close the proxy even if a hold was left inconsistent — never leak two
    // x264 encodes past the session.
    sink_.Stop();
    proxy_running_ = false;
    active_match_.clear();
}

auto ProxyLifecycle::IsProxyRunning() const -> bool {
    std::lock_guard lock(mtx_);
    return proxy_running_;
}

}  // namespace sst::storage
