#pragma once

#include <cstdint>

namespace sst::health {

// Frame-truth per-camera health (state-health cycle U3). Derived from actual
// frame flow (last-sample age + watchdog restart outcomes), never from pipeline
// control state — a stalled-but-"running" capture reads unhealthy here.
//
//   kOk         — frames arrived within the stall threshold.
//   kRecovering — frames stalled; the producer watchdog owns recovery. Entered
//                 immediately on a stall so transient Argus drops never flap
//                 the app UI through DOWN.
//   kDown       — the watchdog completed N restart attempts for THIS camera
//                 and all failed (a count, not an elapsed window: restarts
//                 serialize across cameras, so a queued camera must not be
//                 declared dead while waiting its turn). Frames resuming
//                 return the camera to kOk.
enum class CameraHealth : std::uint8_t {
    kOk = 0,
    kRecovering = 1,
    kDown = 2,
};

}  // namespace sst::health
