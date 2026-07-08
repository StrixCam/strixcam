#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app/focus/ports/focuser.hpp"
#include "app/pipeline/ports/camera-frame-tap.hpp"
#include "domain/focus/models/focus-state.hpp"
#include "domain/health/models/camera-health.hpp"

namespace sst::focus {

struct AutofocusConfig {
    // Loop tick — also the sharpness sample cadence (a few Hz: near-zero CPU,
    // and a full tick between the VCM write and the scored sample gives the
    // lens time to settle before its position is judged).
    std::chrono::milliseconds tick_interval{kDefaultTickIntervalMs};
    // How long one sample grab waits for the camera's next produced frame.
    // Must stay below tick_interval so a stalled camera can't slow the tick.
    std::chrono::milliseconds sample_timeout{kDefaultSampleTimeoutMs};
    // Two-stage sweep: coarse over the full VCM range, then fine around the
    // coarse peak (±coarse_step in fine_step increments).
    std::uint32_t coarse_step{kDefaultCoarseStep};
    std::uint32_t fine_step{kDefaultFineStep};
    // Sweep early-exit: once a stage has a peak, this many CONSECUTIVE worse
    // samples end the stage without scanning the remaining range.
    int worse_limit{kDefaultWorseLimit};
    // Hold hysteresis: re-sweep only after retrigger_consecutive CONSECUTIVE
    // samples below retrigger_ratio × the converged score. Plateau noise and
    // single-frame flukes never accumulate toward a re-trigger, so a converged
    // lens does not oscillate.
    double retrigger_ratio{kDefaultRetriggerRatio};
    int retrigger_consecutive{kDefaultRetriggerConsecutive};
    // Backoff after a failed VCM I2C write before that camera is retried.
    std::chrono::milliseconds i2c_backoff{kDefaultI2cBackoffMs};
    // AF while a recording/streaming session is active. Default ON (metal
    // validation flipped the original R15 stance): converge-and-hold with
    // hysteresis doesn't hunt mid-match, and a match that drifts out of focus
    // is worse than a rare re-sweep. Env SST_AF_DISABLE_DURING_RECORDING=1
    // restores the old pause-while-recording behavior at boot (rollback hatch).
    bool af_during_recording{true};
    std::size_t camera_count{FocusState::kCameras};

   private:
    static constexpr int kDefaultTickIntervalMs = 200;  // 5 Hz sample rate
    static constexpr int kDefaultSampleTimeoutMs = 150;
    static constexpr std::uint32_t kDefaultCoarseStep = 80;
    static constexpr std::uint32_t kDefaultFineStep = 20;
    static constexpr int kDefaultWorseLimit = 3;
    static constexpr double kDefaultRetriggerRatio = 0.5;
    static constexpr int kDefaultRetriggerConsecutive = 3;
    static constexpr int kDefaultI2cBackoffMs = 2000;
};

// Continuous autofocus loop (U6). Owns one background thread (cv-interruptible,
// telemetry-probe pattern — never the BLE dispatcher thread) that, for every
// camera in AUTO mode, hill-climbs the VCM position toward the sharpness peak
// (coarse-then-fine sweep, one position per tick), converges, and holds
// WITHOUT further I2C writes until a sustained sharpness drop re-triggers a
// sweep (hysteresis above).
//
// Stand-down rules, checked every tick:
//   - Fixed lens (focuser unavailable): the loop idles entirely — no I2C.
//   - MANUAL mode (FocusState per-camera atomic): any in-flight sweep aborts;
//     the manual position was already applied by the CameraFocus handler.
//   - Recording active with af_during_recording disabled (non-default —
//     SST_AF_DISABLE_DURING_RECORDING=1): sweeps abort, holds keep their
//     position; sweeping resumes when recording stops. By default AF stays
//     active through record/stream.
//   - Camera health != OK: hunting a stalled camera is pointless I2C churn —
//     that camera is skipped until frame truth says it recovered.
//   - A failed VCM write backs the camera off i2c_backoff (log, no crash).
//
// Sharpness samples come from the per-camera producer-path tap at the loop's
// low tick rate — a descriptor-share of an already-materialized frame, never
// per-frame hot-path work.
class AutofocusService {
   public:
    using HealthProvider = std::function<std::optional<sst::health::CameraHealth>(std::size_t)>;
    using RecordingActiveProvider = std::function<bool()>;

    // health / recording_active may be empty (treated as "always OK" / "never
    // recording"). Reads SST_AF_DISABLE_DURING_RECORDING=1 as a boot-time
    // override of config.af_during_recording (pause AF while recording).
    AutofocusService(IFocuser& focuser, FocusState& focus_state,
                     sst::pipeline::ICameraFrameTap& frame_tap, HealthProvider health,
                     RecordingActiveProvider recording_active, AutofocusConfig config = {});
    ~AutofocusService();

    AutofocusService(const AutofocusService&) = delete;
    auto operator=(const AutofocusService&) -> AutofocusService& = delete;
    AutofocusService(AutofocusService&&) = delete;
    auto operator=(AutofocusService&&) -> AutofocusService& = delete;

    // Idempotent. The loop owns its thread; Stop() joins it promptly (a tick
    // in flight finishes first, bounded by sample_timeout per camera).
    auto Start() -> void;
    auto Stop() -> void;

   private:
    enum class Phase : std::uint8_t {
        kIdle,      // AUTO but no sweep yet (or aborted) — next tick starts one
        kSweeping,  // stepping the VCM through candidate positions
        kHolding,   // converged; watching sharpness, writing nothing
    };

    // Per-camera loop state. Touched only by the loop thread — no locks.
    struct CameraState {
        Phase phase{Phase::kIdle};
        // Sweep progress. `target` is the position commanded last tick; its
        // settled frame is scored this tick (awaiting_sample gates that).
        bool fine_stage{false};
        bool awaiting_sample{false};
        std::uint32_t target{0};
        std::uint32_t sweep_end{0};
        std::uint32_t step{0};
        double best_score{-1.0};
        std::uint32_t best_position{0};
        int worse_streak{0};
        // Hold state.
        double converged_score{0.0};
        int drop_streak{0};
        // I2C failure backoff.
        std::chrono::steady_clock::time_point backoff_until;
    };

    auto Loop() -> void;
    auto Tick() -> void;
    auto TickCamera(std::size_t camera, CameraState& state) -> void;
    auto BeginSweep(std::size_t camera, CameraState& state) -> void;
    auto SweepTick(std::size_t camera, CameraState& state) -> void;
    auto HoldTick(std::size_t camera, CameraState& state) -> void;
    // SetFocus with the stand-down + failure policy applied: skips (returns
    // false) if the camera flipped to MANUAL since the tick started; on an I2C
    // write failure arms backoff_until and returns false.
    auto CommandPosition(std::size_t camera, CameraState& state, std::uint32_t position) -> bool;
    // Grab one tapped frame and score it; nullopt when no frame arrived in
    // time or the frame was unscoreable (retry next tick).
    auto SampleSharpness(std::size_t camera) -> std::optional<double>;

    IFocuser& focuser_;
    FocusState& focus_state_;
    sst::pipeline::ICameraFrameTap& frame_tap_;
    HealthProvider health_;
    RecordingActiveProvider recording_active_;
    AutofocusConfig config_;

    std::vector<CameraState> states_;

    std::mutex run_mtx_;
    std::condition_variable run_cv_;
    bool running_{false};
    std::thread thread_;
};

}  // namespace sst::focus
