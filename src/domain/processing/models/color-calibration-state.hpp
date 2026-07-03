#pragma once

#include <atomic>

namespace sst::processing {

// Live white-balance gains shared between the SetCameraCalibration BLE handler
// (the writer — diagnostic screen sliders) and the postprocessor consumer thread
// (the reader — samples them once per frame). Lock-free atomics: the read sits on
// the hot per-frame path, and a torn read across the four fields is harmless (one
// frame with a mixed gain during a slider drag is invisible).
class ColorCalibrationState {
   public:
    struct Gains {
        float r;
        float g;
        float b;
        bool enabled;
    };

    explicit ColorCalibrationState(Gains initial)
        : r_(initial.r), g_(initial.g), b_(initial.b), enabled_(initial.enabled) {}

    auto Set(Gains gains) -> void {
        r_.store(gains.r, std::memory_order_relaxed);
        g_.store(gains.g, std::memory_order_relaxed);
        b_.store(gains.b, std::memory_order_relaxed);
        enabled_.store(gains.enabled, std::memory_order_relaxed);
    }

    [[nodiscard]] auto Get() const -> Gains {
        return {r_.load(std::memory_order_relaxed), g_.load(std::memory_order_relaxed),
                b_.load(std::memory_order_relaxed), enabled_.load(std::memory_order_relaxed)};
    }

   private:
    std::atomic<float> r_;
    std::atomic<float> g_;
    std::atomic<float> b_;
    std::atomic<bool> enabled_;
};

}  // namespace sst::processing
