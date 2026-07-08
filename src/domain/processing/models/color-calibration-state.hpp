#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace sst::processing {

// Live image-tuning parameters, held PER CAMERA. Writers: the
// SetCameraCalibration BLE handler (diagnostic screen sliders — writes every
// camera identically) and the continuous auto-WB loop (AutoColorService —
// writes each camera's own grey-world gains). Reader: the postprocessor
// consumer thread, which samples the producing camera's gains once per frame.
// Lock-free atomics: the read sits on the hot per-frame path, and a torn read
// across fields is harmless (one frame with a mixed value during a slider
// drag / auto step is invisible).
//
// WB gains fix the per-module cast; saturation/contrast/brightness are the
// rest of the image tuning (the ArduCAM ISP renders flat/washed). All applied
// in the postprocessor after demosaic, so all are live-adjustable without a
// pipeline restart (unlike the capture-level TNR/edge-enhance, which stay
// env-configured).
class ColorCalibrationState {
   public:
    static constexpr std::size_t kCameras = 2;

    struct Gains {
        float r{1.0F};
        float g{1.0F};
        float b{1.0F};
        bool enabled{true};
        float saturation{1.0F};  // 0=grey, 1=unchanged, >1 vivid
        float contrast{1.0F};    // 1=unchanged; scales around mid-grey
        float brightness{0.0F};  // 0=unchanged; additive fraction of full range
    };

    explicit ColorCalibrationState(Gains initial) { Set(initial); }

    // Writes every camera — the manual-calibration surface (one slider set
    // applies rig-wide) and the boot seed.
    auto Set(Gains gains) -> void {
        for (std::size_t camera = 0; camera < kCameras; ++camera) {
            SetCamera(camera, gains);
        }
    }

    // Per-camera write (auto-WB loop). Out-of-range camera is a no-op.
    auto SetCamera(std::size_t camera, Gains gains) -> void {
        if (camera >= kCameras) {
            return;
        }
        auto& slot = cameras_.at(camera);
        slot.r.store(gains.r, std::memory_order_relaxed);
        slot.g.store(gains.g, std::memory_order_relaxed);
        slot.b.store(gains.b, std::memory_order_relaxed);
        slot.enabled.store(gains.enabled, std::memory_order_relaxed);
        slot.saturation.store(gains.saturation, std::memory_order_relaxed);
        slot.contrast.store(gains.contrast, std::memory_order_relaxed);
        slot.brightness.store(gains.brightness, std::memory_order_relaxed);
    }

    // Out-of-range camera reads camera 0 (the shared-manual view).
    [[nodiscard]] auto Get(std::size_t camera = 0) const -> Gains {
        const auto& slot = cameras_.at(camera < kCameras ? camera : 0);
        return {slot.r.load(std::memory_order_relaxed),
                slot.g.load(std::memory_order_relaxed),
                slot.b.load(std::memory_order_relaxed),
                slot.enabled.load(std::memory_order_relaxed),
                slot.saturation.load(std::memory_order_relaxed),
                slot.contrast.load(std::memory_order_relaxed),
                slot.brightness.load(std::memory_order_relaxed)};
    }

   private:
    struct Slot {
        std::atomic<float> r{1.0F};
        std::atomic<float> g{1.0F};
        std::atomic<float> b{1.0F};
        std::atomic<bool> enabled{true};
        std::atomic<float> saturation{1.0F};
        std::atomic<float> contrast{1.0F};
        std::atomic<float> brightness{0.0F};
    };

    std::array<Slot, kCameras> cameras_{};
};

}  // namespace sst::processing
