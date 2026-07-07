#pragma once

namespace sst::session {

// SESSION-END fan-out targets (not connection-loss: a BLE disconnect keeps the
// session alive — only FinalizeSession invokes these). Order-independent; each
// must be idempotent and a no-op when the corresponding subsystem is inactive.
// The concrete implementation wires these to the recording service (finalize
// the continuous MP4 + flush the overlay timeline + stop the raw proxy), the
// streaming service (close RTMP + RTSP), and the WiFi Direct controller (tear
// down the group).
class ISessionCleanup {
   public:
    virtual ~ISessionCleanup() = default;

    // Finalize any active recording (EOS to a playable MP4 + thumbnail).
    // Returns true when an active recording was finalized successfully; false
    // when there was nothing to finalize or the finalize failed. The session
    // manager uses this to fill the summary's file-valid bit when the session
    // ends mid-recording.
    virtual auto FinalizeRecording() -> bool = 0;
    virtual auto StopStreaming() -> void = 0;
    virtual auto TeardownWifiDirect() -> void = 0;

    // Reset session-scoped UI selections (active output camera, preview layout)
    // to their defaults. Runs at SESSION END only — selections survive a mere
    // disconnect so a rejoining app can adopt them from the snapshot (U2).
    // Device-intrinsic state (lens calibration) is deliberately NOT reset.
    virtual auto ResetSelections() -> void = 0;
};

}  // namespace sst::session
