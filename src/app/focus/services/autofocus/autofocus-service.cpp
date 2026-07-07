#include "app/focus/services/autofocus/autofocus-service.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "domain/focus/services/sharpness.hpp"

namespace sst::focus {

AutofocusService::AutofocusService(IFocuser& focuser, FocusState& focus_state,
                                   sst::pipeline::ICameraFrameTap& frame_tap, HealthProvider health,
                                   RecordingActiveProvider recording_active, AutofocusConfig config)
    : focuser_(focuser),
      focus_state_(focus_state),
      frame_tap_(frame_tap),
      health_(std::move(health)),
      recording_active_(std::move(recording_active)),
      config_(config),
      states_(config.camera_count) {
    // Boot-time override (origin R15): AF stays paused during recording by
    // default; SST_AF_DURING_RECORDING=1 keeps it hunting through a match so
    // metal validation can decide whether the default flips.
    if (const char* env = std::getenv("SST_AF_DURING_RECORDING"); env != nullptr) {
        config_.af_during_recording = std::string_view{env} == "1";
    }
}

AutofocusService::~AutofocusService() { Stop(); }

auto AutofocusService::Start() -> void {
    // Assign thread_ under run_mtx_ so a concurrent Stop() can never observe
    // running_==true with an unassigned thread_ (telemetry-probe pattern).
    const std::lock_guard lock(run_mtx_);
    if (running_) {
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
}

auto AutofocusService::Stop() -> void {
    {
        const std::lock_guard lock(run_mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    run_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

auto AutofocusService::Loop() -> void {
    std::unique_lock lock(run_mtx_);
    while (running_) {
        lock.unlock();
        Tick();
        lock.lock();
        // Wait out the tick interval, waking immediately on Stop() so shutdown
        // is prompt (bounded only by an in-flight Tick()'s sample grabs).
        if (run_cv_.wait_for(lock, config_.tick_interval, [this] { return !running_; })) {
            break;
        }
    }
}

auto AutofocusService::Tick() -> void {
    // Fixed lens: no VCM to drive — the loop idles entirely, forever cheap.
    if (!focuser_.IsAvailable()) {
        return;
    }
    const bool paused = !config_.af_during_recording && recording_active_ && recording_active_();
    for (std::size_t camera = 0; camera < states_.size(); ++camera) {
        auto& state = states_[camera];
        if (paused) {
            // Recording-active pause (origin R15). Abort an in-flight sweep
            // rather than freezing it — by the time recording stops the scene
            // is a match later, so a half-done sweep would converge on stale
            // scores. A converged hold keeps its position untouched; sweeping
            // resumes fresh when recording stops.
            if (state.phase == Phase::kSweeping) {
                state = CameraState{};
            }
            continue;
        }
        TickCamera(camera, state);
    }
}

auto AutofocusService::TickCamera(std::size_t camera, CameraState& state) -> void {
    // Manual preemption: the CameraFocus handler set MANUAL (and already drove
    // the VCM to the manual position on the dispatcher thread) — abort any
    // sweep and stand down while the manual position holds.
    if (!focus_state_.IsAuto(static_cast<std::uint32_t>(camera))) {
        if (state.phase != Phase::kIdle) {
            spdlog::info("autofocus: camera {} preempted by manual focus — standing down", camera);
            state = CameraState{};
        }
        return;
    }
    // Don't hunt a camera whose frame truth is not OK (U3): a RECOVERING/DOWN
    // camera produces no scoreable frames, so sweeping it is pure I2C churn.
    if (health_) {
        const auto health = health_(camera);
        if (!health.has_value() || *health != sst::health::CameraHealth::kOk) {
            if (state.phase == Phase::kSweeping) {
                spdlog::info("autofocus: camera {} unhealthy mid-sweep — aborting sweep", camera);
                state = CameraState{};
            }
            return;
        }
    }
    if (std::chrono::steady_clock::now() < state.backoff_until) {
        return;
    }
    switch (state.phase) {
        case Phase::kIdle:
            BeginSweep(camera, state);
            break;
        case Phase::kSweeping:
            SweepTick(camera, state);
            break;
        case Phase::kHolding:
            HoldTick(camera, state);
            break;
    }
}

auto AutofocusService::BeginSweep(std::size_t camera, CameraState& state) -> void {
    state = CameraState{};
    state.phase = Phase::kSweeping;
    state.step = config_.coarse_step;
    state.target = 0;
    state.sweep_end = kMaxFocus;
    spdlog::info("autofocus: camera {} sweep started (coarse step {})", camera, state.step);
    // Drive to the first candidate now; its settled frame is scored next tick.
    state.awaiting_sample = CommandPosition(camera, state, state.target);
}

auto AutofocusService::SweepTick(std::size_t camera, CameraState& state) -> void {
    if (!state.awaiting_sample) {
        // The last command failed (I2C backoff) or no frame confirmed it —
        // re-command the current target and score it next tick.
        state.awaiting_sample = CommandPosition(camera, state, state.target);
        return;
    }
    // Score the position commanded last tick (the lens had a full tick to
    // settle). No frame / unscoreable frame → retry next tick.
    const auto score = SampleSharpness(camera);
    if (!score.has_value()) {
        return;
    }
    if (*score > state.best_score) {
        state.best_score = *score;
        state.best_position = state.target;
        state.worse_streak = 0;
    } else {
        ++state.worse_streak;
    }

    const bool stage_done =
        state.target >= state.sweep_end || state.worse_streak >= config_.worse_limit;
    if (!stage_done) {
        state.target = std::min(state.target + state.step, state.sweep_end);
        state.awaiting_sample = CommandPosition(camera, state, state.target);
        return;
    }
    if (!state.fine_stage) {
        // Fine stage: rescan around the coarse peak with the fine step. The
        // best is rescored from scratch — the fine window contains the coarse
        // best position, and carrying its score would make the ascent from the
        // window's low edge count as a "worse streak" and end the stage before
        // it ever reached the peak.
        state.fine_stage = true;
        const std::uint32_t fine_start = state.best_position > config_.coarse_step
                                             ? state.best_position - config_.coarse_step
                                             : 0;
        state.sweep_end = std::min(state.best_position + config_.coarse_step, kMaxFocus);
        state.step = config_.fine_step;
        state.target = fine_start;
        state.best_score = -1.0;
        state.worse_streak = 0;
        state.awaiting_sample = CommandPosition(camera, state, state.target);
        return;
    }
    // Both stages done: drive to the peak and hold. On a failed write the
    // backoff holds the phase; this branch re-runs (idempotently) after it.
    if (!CommandPosition(camera, state, state.best_position)) {
        return;
    }
    state.phase = Phase::kHolding;
    state.converged_score = state.best_score;
    state.drop_streak = 0;
    spdlog::info("autofocus: camera {} converged at position {} (score {:.1f})", camera,
                 state.best_position, state.best_score);
}

auto AutofocusService::HoldTick(std::size_t camera, CameraState& state) -> void {
    // Converged: watch sharpness, WRITE NOTHING. Only a sustained drop below
    // the hysteresis floor re-triggers a sweep — plateau noise resets the
    // streak, so a still scene never oscillates the lens.
    const auto score = SampleSharpness(camera);
    if (!score.has_value()) {
        return;
    }
    if (*score < state.converged_score * config_.retrigger_ratio) {
        ++state.drop_streak;
    } else {
        state.drop_streak = 0;
    }
    if (state.drop_streak >= config_.retrigger_consecutive) {
        spdlog::info(
            "autofocus: camera {} sharpness dropped (score {:.1f} vs converged {:.1f}) — "
            "re-triggering sweep",
            camera, *score, state.converged_score);
        state = CameraState{};  // kIdle → fresh sweep next tick
    }
}

auto AutofocusService::CommandPosition(std::size_t camera, CameraState& state,
                                       std::uint32_t position) -> bool {
    // Re-check AUTO right before the write: a manual command dispatched since
    // this tick started must win — its position was already applied, so the
    // sweep must not overwrite it. (TickCamera aborts the sweep next tick.)
    if (!focus_state_.IsAuto(static_cast<std::uint32_t>(camera))) {
        return false;
    }
    if (focuser_.SetFocus(static_cast<std::uint32_t>(camera), position)) {
        return true;
    }
    state.backoff_until = std::chrono::steady_clock::now() + config_.i2c_backoff;
    spdlog::warn("autofocus: camera {} VCM write failed — backing off {}ms", camera,
                 config_.i2c_backoff.count());
    return false;
}

auto AutofocusService::SampleSharpness(std::size_t camera) -> std::optional<double> {
    const auto frame = frame_tap_.GrabCameraFrame(camera, config_.sample_timeout);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return SharpnessScore(*frame);
}

}  // namespace sst::focus
