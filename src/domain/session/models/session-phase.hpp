#pragma once

#include <cstdint>

namespace sst::session {

// The SESSION axis of the orthogonal state model (app_connected ⟂ wifi_group ⟂
// session). Unlike the old single phase ladder, this axis is independent of the
// BLE connection: a disconnect no longer moves it, so a session (and its
// recording) outlives the app.
//
// Forward edges: Idle → Configured → Ready → Recording. Recording drops back to
// Ready on a commanded stop. Any active state (≥ Configured) ends through
// Finalizing — claimed exactly once by compare-and-swap under the session mutex
// (app stop / auto-stop / camera failure / shutdown) — and lands back on Idle.
enum class SessionPhase : std::uint8_t {
    kIdle = 0,        // no session (config not applied, or the last one finalized)
    kConfigured = 1,  // session config received + output dirs prepared
    kReady = 2,       // overlay layout applied — ready to record/stream
    kRecording = 3,   // recording in progress
    kFinalizing = 4,  // session end claimed; cleanup fan-out in flight
};

}  // namespace sst::session
