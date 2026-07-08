#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app/pipeline/ports/camera-frame-tap.hpp"
#include "domain/health/models/camera-health.hpp"
#include "domain/processing/models/auto-color-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/utils/auto-color.hpp"

namespace sst::processing {

struct AutoColorConfig {
    // Loop tick — the color-stats sample cadence. Slow on purpose: color
    // drifts over seconds (venue light), not frames, and a slow cadence plus
    // damping is what makes the correction invisible instead of a pump.
    std::chrono::milliseconds tick_interval{kDefaultTickIntervalMs};
    // How long one sample grab waits for a camera's next produced frame. Kept
    // small so a stalled camera can never stretch the tick.
    std::chrono::milliseconds sample_timeout{kDefaultSampleTimeoutMs};
    // Grab attempts per camera per tick. The producer tap is shared with the
    // autofocus sampler and a contended grab can lose the race (metal-observed:
    // one camera's grabs losing most ticks) — a couple of retries makes a
    // sample per tick the norm again.
    int sample_attempts{kDefaultSampleAttempts};
    // How long a camera's last measured means stay usable for the SHARED
    // target when a tick misses a fresh sample. Without this, one missed grab
    // flips the target between "both cameras" and "one camera" and the gains
    // pump (metal-observed ~10-count target flap). Venue color drifts over
    // tens of seconds, so briefly-stale means are still true.
    std::chrono::milliseconds means_staleness{kDefaultMeansStalenessMs};
    // Grey-world step tuning (damping / dead-band / clamps) — domain math.
    AutoColorTuning tuning{};
    std::size_t camera_count{ColorCalibrationState::kCameras};

   private:
    static constexpr int kDefaultTickIntervalMs = 3000;
    static constexpr int kDefaultSampleTimeoutMs = 150;
    static constexpr int kDefaultSampleAttempts = 3;
    static constexpr int kDefaultMeansStalenessMs = 20000;
};

// Continuous software auto white-balance — out-of-the-box color in any venue.
// Owns one low-rate background thread (cv-interruptible, telemetry-probe
// pattern) that each tick samples BOTH cameras' pre-correction channel means
// via the per-camera producer tap, derives one SHARED neutral grey target,
// and pulls each camera's postprocess WB gains a damped step toward it. Both
// cameras chasing the same target keeps them matched in any light; the
// dead-band means a converged loop writes (and logs) nothing.
//
// Stand-down rules, checked every tick:
//   - AutoColorMode::kManual (user slider calibration or one-shot auto-WB
//     command applied): the loop stands down until the app hands authority
//     back with SetCameraCalibration(enabled=false) — manual preemption, same
//     discipline as manual focus.
//   - Camera health != OK: that camera is skipped (its gains hold; the other
//     camera still tracks the shared target).
//   - A dark frame (grey-world untrustworthy): skipped, gains hold.
//   - SST_AUTO_COLOR_DISABLE=1 (boot rollback hatch): Start() never spawns
//     the thread — behavior falls back to the static seeded gains.
class AutoColorService {
   public:
    using HealthProvider = std::function<std::optional<sst::health::CameraHealth>(std::size_t)>;

    // `health` may be empty (treated as "always OK").
    AutoColorService(sst::pipeline::ICameraFrameTap& frame_tap, ColorCalibrationState& calibration,
                     AutoColorState& mode, HealthProvider health, AutoColorConfig config = {});
    ~AutoColorService();

    AutoColorService(const AutoColorService&) = delete;
    auto operator=(const AutoColorService&) -> AutoColorService& = delete;
    AutoColorService(AutoColorService&&) = delete;
    auto operator=(AutoColorService&&) -> AutoColorService& = delete;

    // Idempotent. The loop owns its thread; Stop() joins it promptly (a tick
    // in flight finishes first, bounded by sample_timeout per camera).
    auto Start() -> void;
    auto Stop() -> void;

   private:
    // A camera's last successful measurement — loop-thread-only.
    struct CachedMeans {
        std::optional<ChannelMeans> means;
        std::chrono::steady_clock::time_point measured_at;
    };

    auto Loop() -> void;
    auto Tick() -> void;
    // One camera's sampled means, or nullopt (unhealthy / no frame / not NV12).
    auto SampleCamera(std::size_t camera) -> std::optional<ChannelMeans>;

    sst::pipeline::ICameraFrameTap& frame_tap_;
    ColorCalibrationState& calibration_;
    AutoColorState& mode_;
    HealthProvider health_;
    AutoColorConfig config_;
    bool disabled_{false};
    std::vector<CachedMeans> cache_;

    std::mutex run_mtx_;
    std::condition_variable run_cv_;
    bool running_{false};
    std::thread thread_;
};

}  // namespace sst::processing
