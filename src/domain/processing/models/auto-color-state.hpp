#pragma once

#include <atomic>
#include <cstdint>

namespace sst::processing {

// Auto-vs-manual color authority — the same intent-vs-observed discipline as
// focus (FocusState): a user-set manual calibration PREEMPTS the continuous
// auto-WB loop, and an explicit re-enable hands authority back.
//
//   kAuto   — boot default. AutoColorService continuously pulls each camera's
//             WB gains toward the shared grey-world target.
//   kManual — a user override holds: SetCameraCalibration(enabled=true) slider
//             values or a one-shot AutoWhiteBalance result. The loop stands
//             down so it never fights (or silently overwrites) the user's
//             values. SetCameraCalibration(enabled=false) returns to kAuto.
enum class AutoColorMode : std::uint8_t {
    kAuto = 0,
    kManual = 1,
};

// Written by the calibration BLE handlers, read by the auto-WB loop each tick.
// A single scalar — lock-free.
class AutoColorState {
   public:
    auto SetMode(AutoColorMode mode) -> void { mode_.store(mode, std::memory_order_relaxed); }

    [[nodiscard]] auto Mode() const -> AutoColorMode {
        return mode_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<AutoColorMode> mode_{AutoColorMode::kAuto};
};

}  // namespace sst::processing
