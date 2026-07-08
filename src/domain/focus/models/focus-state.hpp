#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sst::focus {

// Per-camera focus mode + last-commanded position. Written by the CameraFocus BLE
// handler, read by the autofocus loop (which hunts only cameras in AUTO mode and
// stands down while a manual position holds). Lock-free — mode/position are
// single scalars and a torn read across cameras is harmless.
class FocusState {
   public:
    static constexpr std::size_t kCameras = 2;

    auto SetAuto(std::uint32_t camera, bool is_auto) -> void {
        if (camera < kCameras) {
            auto_[camera].store(is_auto, std::memory_order_relaxed);
        }
    }
    auto SetManualPosition(std::uint32_t camera, std::uint32_t position) -> void {
        if (camera < kCameras) {
            position_[camera].store(position, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto IsAuto(std::uint32_t camera) const -> bool {
        return camera < kCameras && auto_[camera].load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto Position(std::uint32_t camera) const -> std::uint32_t {
        return camera < kCameras ? position_[camera].load(std::memory_order_relaxed) : 0;
    }

   private:
    // Boot default: AUTO for every camera (matches the proto FOCUS_MODE_AUTO
    // default) — continuous AF runs from boot without waiting for an app
    // command. The CameraFocus handler flips a camera to manual on demand.
    std::array<std::atomic<bool>, kCameras> auto_{true, true};
    std::array<std::atomic<std::uint32_t>, kCameras> position_{};
};

}  // namespace sst::focus
