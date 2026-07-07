#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "app/raw_capture/ports/raw-capture-sink.hpp"

namespace sst::raw_capture {

// Record-or-stream ref-count for the training proxy: the proxy runs while a
// recording OR a platform (RTMP) stream is active and stops on last-out, so
// streaming-only sessions still produce training footage. The always-on RTSP
// preview deliberately does NOT hold the proxy — pre-match framing must leave
// the proxy (and its two extra x264 encodes) off.
//
// One shared instance, driven by RecordingHandler (record leg), StreamingHandler
// (stream leg), and SessionCleanup (ForceStop at session end). All transitions
// happen under one mutex, so rapid start/stop interleavings can never
// double-start the sink or orphan a running proxy; every leg is idempotent.
//
// Proxy identity: a record START carrying the app-minted capture_group_id
// starts (or rebinds) the proxy under that id in the per-match directory — the
// id is the app's download-grouping contract, so it wins over an interim id
// even at the cost of a momentary proxy restart. A stream-only start mints an
// interim `stream-<epoch-ms>` id at the sink's root (there is no per-match dir
// without a record). A record START with no group id takes the record HOLD but
// starts nothing (pre-coupling apps record without training footage — existing
// contract).
class ProxyLifecycle {
   public:
    // `now_ms`: epoch-ms clock for minting interim stream-only group ids.
    // Defaults to the system clock; injectable for tests.
    using Clock = std::function<std::uint64_t()>;

    explicit ProxyLifecycle(IRawCaptureSink& sink, Clock now_ms = nullptr);

    // Record leg. Empty `capture_group_id` => hold only (no proxy identity from
    // this record). Proxy-start failure is non-fatal by design (training footage
    // is best-effort) — it is logged and the record proceeds.
    auto OnRecordingStart(const std::string& capture_group_id,
                          const std::filesystem::path& output_dir) -> void;
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
    // observability; telemetry keeps reading the sink's own IsCapturing()).
    [[nodiscard]] auto IsProxyRunning() const -> bool;

   private:
    // Starts the sink under `group_id` into `output_dir` (empty dir => sink
    // root). Caller holds mtx_.
    auto StartLocked(const std::string& group_id, const std::filesystem::path& output_dir) -> void;
    // Stops the sink when no leg holds it anymore. Caller holds mtx_.
    auto StopIfIdleLocked() -> void;

    IRawCaptureSink& sink_;
    Clock now_ms_;

    mutable std::mutex mtx_;
    bool record_active_{false};
    bool stream_active_{false};
    bool proxy_running_{false};
    std::string active_group_;
};

}  // namespace sst::raw_capture
