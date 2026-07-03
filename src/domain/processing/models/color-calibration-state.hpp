#pragma once

#include <atomic>

namespace sst::processing {

// Live image-tuning parameters shared between the SetCameraCalibration BLE handler
// (the writer — diagnostic screen sliders) and the postprocessor consumer thread
// (the reader — samples them once per frame). Lock-free atomics: the read sits on
// the hot per-frame path, and a torn read across fields is harmless (one frame
// with a mixed value during a slider drag is invisible).
//
// WB gains fix the magenta cast; saturation/contrast/brightness are the rest of
// the image tuning (the ArduCAM ISP renders flat/washed). All applied in the
// postprocessor after demosaic, so all are live-adjustable without a pipeline
// restart (unlike the capture-level TNR/edge-enhance, which stay env-configured).
class ColorCalibrationState {
   public:
    struct Gains {
        float r{1.0F};
        float g{1.0F};
        float b{1.0F};
        bool enabled{true};
        float saturation{1.0F};  // 0=grey, 1=unchanged, >1 vivid
        float contrast{1.0F};    // 1=unchanged; scales around mid-grey
        float brightness{0.0F};  // 0=unchanged; additive fraction of full range
    };

    explicit ColorCalibrationState(Gains initial)
        : r_(initial.r),
          g_(initial.g),
          b_(initial.b),
          enabled_(initial.enabled),
          saturation_(initial.saturation),
          contrast_(initial.contrast),
          brightness_(initial.brightness) {}

    auto Set(Gains gains) -> void {
        r_.store(gains.r, std::memory_order_relaxed);
        g_.store(gains.g, std::memory_order_relaxed);
        b_.store(gains.b, std::memory_order_relaxed);
        enabled_.store(gains.enabled, std::memory_order_relaxed);
        saturation_.store(gains.saturation, std::memory_order_relaxed);
        contrast_.store(gains.contrast, std::memory_order_relaxed);
        brightness_.store(gains.brightness, std::memory_order_relaxed);
    }

    [[nodiscard]] auto Get() const -> Gains {
        return {r_.load(std::memory_order_relaxed),          g_.load(std::memory_order_relaxed),
                b_.load(std::memory_order_relaxed),          enabled_.load(std::memory_order_relaxed),
                saturation_.load(std::memory_order_relaxed), contrast_.load(std::memory_order_relaxed),
                brightness_.load(std::memory_order_relaxed)};
    }

   private:
    std::atomic<float> r_;
    std::atomic<float> g_;
    std::atomic<float> b_;
    std::atomic<bool> enabled_;
    std::atomic<float> saturation_;
    std::atomic<float> contrast_;
    std::atomic<float> brightness_;
};

}  // namespace sst::processing
