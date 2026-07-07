#pragma once

#include <functional>

#include "domain/session/models/live-match.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/session/models/session-phase.hpp"
#include "domain/session/models/session-state.hpp"
#include "domain/session/models/session-summary.hpp"

namespace sst::session {

// Owns the in-memory session state as three orthogonal axes: app_connected
// (BLE), wifi_group (up/down), and the session axis (SessionPhase). Sessions
// OUTLIVE connections: OnConnect is accepted in any session state and cancels
// the auto-stop safety net; OnDisconnect keeps an active session (and its WiFi
// group) alive and arms the auto-stop timer instead of tearing down. A session
// ends only through FinalizeSession — claimed exactly once (CAS to Finalizing
// under the session mutex) by app stop, auto-stop, camera failure, or firmware
// shutdown — which fans out cleanup, records the last-session summary, and
// tears down the group. Implementations are thread-safe: the BLE thread, event
// handlers, and the auto-stop timer thread call in concurrently.
class ISessionManager {
   public:
    virtual ~ISessionManager() = default;

    // The session axis. Connection and WiFi-group state live in Snapshot().
    [[nodiscard]] virtual auto Phase() const -> SessionPhase = 0;
    [[nodiscard]] virtual auto Snapshot() const -> SessionState = 0;

    // Connection axis. OnConnect is accepted in ANY session state (a reconnect
    // into a running session is the norm) and cancels the auto-stop timer.
    // OnDisconnect arms the auto-stop timer when a session is active or the
    // WiFi group is up; it never finalizes or tears down by itself.
    virtual auto OnConnect() -> bool = 0;
    virtual auto OnDisconnect() -> void = 0;

    // WiFi-group axis. OnWifiStopped while a session is active finalizes the
    // session FIRST (the group going down mid-recording must not lose the file).
    virtual auto OnWifiReady() -> bool = 0;
    virtual auto OnWifiStopped() -> bool = 0;

    // Session axis. Each returns true if the transition was allowed in the
    // current state, false (and a no-op) otherwise.
    virtual auto ApplySessionConfig(const SessionConfig& config) -> bool = 0;
    virtual auto OnOverlayConfigured() -> bool = 0;
    virtual auto OnRecordingStart() -> bool = 0;
    virtual auto OnRecordingStop() -> bool = 0;

    // End the active session: CAS the session axis to Finalizing (losers of the
    // race return false immediately — one finalize, ever), fan out cleanup
    // (finalize recording, stop streams, reset selections, tear down the WiFi
    // group), record + persist the last-session summary, and reset to Idle.
    virtual auto FinalizeSession(SessionEndReason reason) -> bool = 0;

    // Apply a display-only live-match mutation (score/clock/period) while a
    // session is active (Configured or beyond, not Finalizing). Returns false
    // (no-op) if there is no active session.
    virtual auto ApplyMatchUpdate(const std::function<void(LiveMatch&)>& mutate) -> bool = 0;
};

}  // namespace sst::session
