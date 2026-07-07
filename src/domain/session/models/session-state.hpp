#pragma once

#include <cstdint>
#include <optional>

#include "domain/session/models/live-match.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/session/models/session-phase.hpp"
#include "domain/session/models/session-summary.hpp"

namespace sst::session {

// The complete in-memory session state, expressed as three ORTHOGONAL axes:
// `app_connected` (BLE central present), `wifi_group_up` (WiFi-Direct group
// formed), and `phase` (the session axis). A BLE disconnect flips only
// `app_connected` — an active session and its WiFi group survive it, guarded by
// the auto-stop timer. `config` is set once PushSessionConfig arrives; `match`
// accumulates display-only score/clock; `last_summary` describes the previous
// session and survives across Idle until the next session end overwrites it.
struct SessionState {
    bool app_connected{false};
    bool wifi_group_up{false};
    SessionPhase phase{SessionPhase::kIdle};
    std::optional<SessionConfig> config;
    LiveMatch match;
    // Monotonic recording duration (steady_clock; survives disconnects). Filled
    // at Snapshot() time for the U2 session snapshot.
    std::uint32_t recording_elapsed_seconds{0};
    std::optional<LastSessionSummary> last_summary;
};

}  // namespace sst::session
