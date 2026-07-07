#pragma once

#include <cstdint>
#include <string>

namespace sst::session {

// Why the last session ended. kReboot means the firmware went down while the
// session was still active (crash/power loss) and the session was reconciled
// from the write-ahead record at the next boot, not finalized live.
enum class SessionEndReason : std::uint8_t {
    kAppStop = 0,        // the app ended the session (StopWifiDirect / shutdown)
    kAutoStop = 1,       // the no-app auto-stop safety net fired
    kCameraFailure = 2,  // a camera went DOWN mid-recording (U3 hook)
    kReboot = 3,         // firmware restarted while the session was active
};

// Summary of the most recently ended session. Survives across Idle in memory
// and as a small JSON in the config dir (so a crash mid-recording still leaves
// a record), until the next session end overwrites it. The app reads it from
// the session snapshot after a reconnect to explain what happened while it was
// away (U2 wire exposure).
struct LastSessionSummary {
    std::string match_uuid;
    SessionEndReason end_reason{SessionEndReason::kAppStop};
    // Display match clock (LiveMatch.clock_seconds) at session end — what the
    // scoreboard read when the session ended.
    std::uint32_t end_clock_seconds{0};
    // True when the session's recording was finalized to a playable MP4 (clean
    // EOS + moov). False when nothing was recorded or the file was orphaned.
    bool file_valid{false};
};

}  // namespace sst::session
