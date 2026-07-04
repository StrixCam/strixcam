#pragma once

namespace sst::session {

// Disconnect/teardown fan-out targets (R15, AE2). On session end the
// SessionManager invokes all of these — order-independent; each must be
// idempotent and a no-op when the corresponding subsystem is inactive. The
// concrete implementation (U14) wires these to the recording service (finalize
// the continuous MP4), the streaming service (close RTMP), and the WiFi Direct
// controller (tear down the group).
class ISessionCleanup {
   public:
    virtual ~ISessionCleanup() = default;

    virtual auto FinalizeRecording() -> void = 0;
    virtual auto StopStreaming() -> void = 0;
    virtual auto TeardownWifiDirect() -> void = 0;

    // Reset session-scoped UI selections (active output camera, preview layout)
    // to their defaults. These are per-connection, not device-intrinsic: a
    // reconnect must start from the app's fresh-UI state (camera 0 / single
    // layout), otherwise the app shows Left while the firmware still serves the
    // previous session's camera — a mismatch the user can only clear by toggling.
    // Device-intrinsic state (lens calibration, R10) is deliberately NOT reset.
    virtual auto ResetSelections() -> void = 0;
};

}  // namespace sst::session
