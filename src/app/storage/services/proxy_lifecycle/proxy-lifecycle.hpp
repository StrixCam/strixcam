#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "app/storage/ports/proxy-sink.hpp"

namespace sst::storage {

// The active session's proxy identity: the app-pushed match_uuid names the
// per-camera proxy files (proxy__<match_uuid>__cam<N>.mp4) and the per-match
// output dir (SessionConfig.video_output_path) places them beside the final
// <match>.mp4. Read through a provider so the lifecycle always sees the
// SessionManager's CURRENT config at start time.
struct ProxySessionInfo {
    std::string match_uuid;
    std::filesystem::path output_dir;
};

// Record-or-stream ref-count for the internal dual-camera proxy — a firmware-
// automatic development artifact (angle/framing/stability/color-grading checks
// and model training), invisible to the app: the proxy runs while a recording
// OR a platform (RTMP) stream is active and stops on last-out, so
// streaming-only matches still produce development footage. The always-on RTSP
// preview deliberately does NOT hold the proxy — pre-match framing must leave
// the proxy (and its two extra x264 encodes) off.
//
// One shared instance, driven by RecordingHandler (record leg), StreamingHandler
// (stream leg), and SessionCleanup (ForceStop at session end). All transitions
// happen under one mutex, so rapid start/stop interleavings can never
// double-start the sink or orphan a running proxy; every leg is idempotent.
//
// Proxy identity: each start consults the session-info provider — the session's
// match_uuid is the on-device pairing key (folder layout; retrieval by ssh), so
// it wins over an interim id even at the cost of a momentary proxy restart on a
// record start (rebind). A stream-only start with no session config mints an
// interim `stream-<epoch-ms>` id at the sink's root (there is no per-match dir
// without a config). A record start with no session info takes the record HOLD
// but starts nothing (there is no match to pair with — defensive; the recording
// handler requires a config before it records).
class ProxyLifecycle {
   public:
    // Reads the active session's match identity (SessionManager has the
    // config). nullopt / empty match_uuid => no session identity available.
    using SessionInfoProvider = std::function<std::optional<ProxySessionInfo>()>;
    // `now_ms`: epoch-ms clock for minting interim stream-only ids. Defaults to
    // the system clock; injectable for tests.
    using Clock = std::function<std::uint64_t()>;

    ProxyLifecycle(IProxySink& sink, SessionInfoProvider session_info, Clock now_ms = nullptr);

    // Record leg. Proxy-start failure is non-fatal by design (development
    // footage is best-effort) — it is logged and the record proceeds.
    auto OnRecordingStart() -> void;
    auto OnRecordingStop() -> void;

    // Stream leg (platform/RTMP egress only).
    auto OnStreamingStart() -> void;
    auto OnStreamingStop() -> void;

    // Session-end force stop: resets BOTH holds atomically with the sink stop,
    // so a session ending without commanded stops (auto-stop, camera failure,
    // shutdown) can't leave a stale hold that would make the next session's
    // first start miss the 0->1 transition.
    auto ForceStop() -> void;

    // True while this lifecycle believes it has the sink running (test/debug
    // observability).
    [[nodiscard]] auto IsProxyRunning() const -> bool;

   private:
    // Starts the sink under `match_uuid` into `output_dir` (empty dir => sink
    // root). Caller holds mtx_.
    auto StartLocked(const std::string& match_uuid,
                     const std::filesystem::path& output_dir) -> void;
    // Stops the sink when no leg holds it anymore. Caller holds mtx_.
    auto StopIfIdleLocked() -> void;
    // The provider's current reading, nullopt when unset / no usable identity.
    [[nodiscard]] auto CurrentSessionInfo() const -> std::optional<ProxySessionInfo>;

    IProxySink& sink_;
    SessionInfoProvider session_info_;
    Clock now_ms_;

    mutable std::mutex mtx_;
    bool record_active_{false};
    bool stream_active_{false};
    bool proxy_running_{false};
    std::string active_match_;
};

}  // namespace sst::storage
