#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "app/session/ports/session-cleanup.hpp"
#include "app/session/ports/session-manager.hpp"
#include "app/session/ports/session-summary-store.hpp"
#include "domain/session/models/session-state.hpp"

namespace sst::session {

// Firmware default for the auto-stop safety net when the app set no
// auto_stop_minutes (origin decision: 30 min, app-configurable via config).
inline constexpr std::chrono::minutes kDefaultAutoStopWindow{30};

// Auto-stop timing knobs. Production uses the defaults; tests shrink both so
// timer scenarios run in milliseconds without changing the arming logic.
struct SessionTiming {
    // Applied when an active session carries no auto_stop_minutes.
    std::chrono::milliseconds default_auto_stop{kDefaultAutoStopWindow};
    // Real duration of one config "minute" (auto_stop_minutes * this).
    std::chrono::milliseconds config_minute{std::chrono::minutes{1}};
    // Grace before an idle preview-only WiFi-Direct group is torn down after the
    // app disconnects. Short so BLE advertising — starved by the 2.4GHz P2P group
    // on the RTL8822CE combo radio (rtk_btcoex) — recovers quickly for
    // rediscovery, but long enough that a brief reconnect blip keeps the group
    // and avoids a disruptive P2P reform (Argus INVALID_SETTINGS). Tests shrink
    // it alongside the auto-stop knobs.
    std::chrono::milliseconds idle_group_grace{std::chrono::seconds{20}};
};

// Concrete session state as three orthogonal axes (see ISessionManager).
// Disconnect keeps an active session + WiFi group alive and arms the auto-stop
// safety net on an internal cv-interruptible timer thread; connect cancels it
// under the same mutex, so exactly one of {cancel, fire} ever wins. Session end
// is claimed once by CAS to Finalizing; the last-session summary survives Idle
// in memory and in the summary store (write-ahead on recording start, so a
// crash mid-recording is reconciled at the next boot's Load()).
class SessionManager final : public ISessionManager {
   public:
    // `store` persists the last-session summary; nullptr = in-memory only
    // (unit tests that don't exercise persistence).
    explicit SessionManager(ISessionCleanup& cleanup, ISessionSummaryStore* store = nullptr,
                            SessionTiming timing = {});
    ~SessionManager() override;

    SessionManager(const SessionManager&) = delete;
    auto operator=(const SessionManager&) -> SessionManager& = delete;
    SessionManager(SessionManager&&) = delete;
    auto operator=(SessionManager&&) -> SessionManager& = delete;

    [[nodiscard]] auto Phase() const -> SessionPhase override;
    [[nodiscard]] auto Snapshot() const -> SessionState override;

    auto OnConnect() -> bool override;
    auto OnDisconnect() -> void override;
    auto OnWifiReady() -> bool override;
    auto OnWifiStopped() -> bool override;
    auto ApplySessionConfig(const SessionConfig& config) -> bool override;
    auto OnOverlayConfigured() -> bool override;
    auto OnRecordingStart() -> bool override;
    auto OnRecordingStop() -> bool override;
    auto FinalizeSession(SessionEndReason reason) -> bool override;

    auto ApplyMatchUpdate(const std::function<void(LiveMatch&)>& mutate) -> bool override;

   private:
    using SteadyClock = std::chrono::steady_clock;

    // Creates the parent directories of the session's video + thumbnail output
    // paths. Returns false if either mkdir fails.
    static auto PrepareOutputDirs(const SessionConfig& config) -> bool;

    // True while a session config is live (Configured/Ready/Recording — the
    // states ApplyMatchUpdate / OnOverlayConfigured build on).
    [[nodiscard]] auto HasActiveConfigLocked() const -> bool;

    // Arm (or re-arm) the deadline `timeout` from now and wake the timer thread.
    // Shared backing for the auto-stop and idle-group-grace timers; the fire
    // handler branches on the live state, not on which duration armed it. Caller
    // holds mtx_.
    auto ArmTimerLocked(std::chrono::milliseconds timeout) -> void;
    // Arm the auto-stop deadline from the session config (default when unset) —
    // the unattended-active-session safety net. Caller holds mtx_.
    auto ArmAutoStopLocked() -> void;
    // Arm the short idle-group teardown grace (timing_.idle_group_grace) so an
    // idle preview-only WiFi group is dropped soon after the app leaves, freeing
    // BLE advertising for rediscovery. Caller holds mtx_.
    auto ArmIdleGroupGraceLocked() -> void;
    // Drop a pending deadline and wake the timer thread. Caller holds mtx_.
    auto CancelAutoStopLocked() -> void;

    // The single finalize gate (CAS to kFinalizing). Caller holds `lock`; the
    // cleanup fan-out runs unlocked and the lock is re-taken to reset to Idle.
    // Returns false when there is no active session or a finalize already won.
    auto FinalizeLocked(std::unique_lock<std::mutex>& lock, SessionEndReason reason) -> bool;

    // Auto-stop fired with the arm still current: finalize an active session,
    // or tear down an idle-with-no-app WiFi group. Caller holds `lock`.
    auto HandleAutoStopLocked(std::unique_lock<std::mutex>& lock) -> void;

    // cv-interruptible deadline loop (telemetry-probe pattern): waits until a
    // deadline is armed, then until it passes or is cancelled/re-armed.
    auto TimerLoop() -> void;

    // Accumulate a running recording segment into recording_accumulated_.
    // Caller holds mtx_.
    auto StopElapsedClockLocked() -> void;

    // Persist `summary` through the store (if any), swallowing nothing — a
    // failure is logged by the store and the in-memory copy stays authoritative.
    auto PersistSummary(const LastSessionSummary& summary) -> void;

    ISessionCleanup& cleanup_;
    ISessionSummaryStore* store_;
    const SessionTiming timing_;

    mutable std::mutex mtx_;
    SessionState state_;

    // Recording elapsed time (monotonic; survives disconnects). accumulated_
    // holds completed segments; started_at_ is set while recording.
    std::chrono::milliseconds recording_accumulated_{0};
    std::optional<SteadyClock::time_point> recording_started_at_;
    // A recording this session was already finalized cleanly (commanded stop),
    // so a later session end reports file-valid even though FinalizeRecording
    // is a no-op by then.
    bool recording_file_valid_{false};

    // Auto-stop timer (guarded by mtx_; timer_cv_ pairs with it).
    std::condition_variable timer_cv_;
    std::optional<SteadyClock::time_point> auto_stop_deadline_;
    bool stop_thread_{false};
    std::thread timer_thread_;
};

}  // namespace sst::session
