#include "app/processing/services/auto-color/auto-color-service.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

namespace sst::processing {

AutoColorService::AutoColorService(sst::pipeline::ICameraFrameTap& frame_tap,
                                   ColorCalibrationState& calibration, AutoColorState& mode,
                                   HealthProvider health, AutoColorConfig config)
    : frame_tap_(frame_tap),
      calibration_(calibration),
      mode_(mode),
      health_(std::move(health)),
      config_(config),
      cache_(config.camera_count) {
    // Boot rollback hatch: pin the seeded static gains, no loop.
    if (std::getenv("SST_AUTO_COLOR_DISABLE") != nullptr) {
        disabled_ = true;
    }
}

AutoColorService::~AutoColorService() { Stop(); }

auto AutoColorService::Start() -> void {
    if (disabled_) {
        spdlog::info("auto-color: disabled via SST_AUTO_COLOR_DISABLE — static gains hold");
        return;
    }
    // Assign thread_ under run_mtx_ so a concurrent Stop() can never observe
    // running_==true with an unassigned thread_ (telemetry-probe pattern).
    const std::lock_guard lock(run_mtx_);
    if (running_) {
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    spdlog::info("auto-color: continuous auto white-balance started (tick {} ms)",
                 config_.tick_interval.count());
}

auto AutoColorService::Stop() -> void {
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

auto AutoColorService::Loop() -> void {
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

auto AutoColorService::Tick() -> void {
    // Manual preemption: a user calibration (sliders or one-shot auto-WB)
    // holds until the app re-enables auto — never fight the user's values.
    if (mode_.Mode() != AutoColorMode::kAuto) {
        return;
    }

    // Sample every camera first: the shared target must come from this tick's
    // view of BOTH cameras, not camera 0's view before camera 1 moved. A tick
    // that misses a fresh sample (the tap is shared with the AF sampler and a
    // contended grab can lose) falls back to that camera's recent cached
    // means — otherwise the shared target flaps between "both cameras" and
    // "one camera" and the gains pump.
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::optional<ChannelMeans>> means(config_.camera_count);
    for (std::size_t camera = 0; camera < config_.camera_count; ++camera) {
        if (auto fresh = SampleCamera(camera); fresh.has_value()) {
            cache_[camera] = {.means = fresh, .measured_at = now};
            means[camera] = fresh;
        } else if (cache_[camera].means.has_value() &&
                   (now - cache_[camera].measured_at) <= config_.means_staleness) {
            means[camera] = cache_[camera].means;
        }
    }
    const auto target = SharedNeutralTarget(means);
    if (!target.has_value()) {
        return;  // no usable sample this tick — every camera's gains hold
    }
    const float shared_target = target.value_or(0.0F);

    for (std::size_t camera = 0; camera < config_.camera_count; ++camera) {
        if (!means[camera].has_value()) {
            continue;
        }
        const ChannelMeans sample = means[camera].value_or(ChannelMeans{});
        const auto current = calibration_.Get(camera);
        const auto stepped = GreyWorldStep(current, sample, shared_target, config_.tuning);
        if (!stepped.has_value()) {
            continue;  // converged (dead-band) or too dark — no write, no log
        }
        const ColorCalibrationState::Gains next = stepped.value_or(ColorCalibrationState::Gains{});
        calibration_.SetCamera(camera, next);
        // info, not debug: an applied step happens only while converging (the
        // dead-band silences steady state), so this is the field-diagnosable
        // convergence trace, a handful of lines per light change.
        spdlog::info(
            "auto-color: camera {} means R={:.1f} G={:.1f} B={:.1f} target={:.1f} -> gains "
            "R={:.3f} G={:.3f} B={:.3f}",
            camera, sample.r, sample.g, sample.b, shared_target, next.r, next.g, next.b);
    }
}

auto AutoColorService::SampleCamera(std::size_t camera) -> std::optional<ChannelMeans> {
    // Don't sample a camera whose frame truth is not OK: a RECOVERING/DOWN
    // camera produces stale/no frames — its gains simply hold.
    if (health_) {
        const auto health = health_(camera);
        if (!health.has_value() || *health != sst::health::CameraHealth::kOk) {
            return std::nullopt;
        }
    }
    // Bounded retries: the tap serves one waiter per frame and the AF sampler
    // shares it, so a single contended grab can lose the wake-up race.
    for (int attempt = 0; attempt < config_.sample_attempts; ++attempt) {
        const auto frame = frame_tap_.GrabCameraFrame(camera, config_.sample_timeout);
        if (frame.has_value()) {
            return MeasureFrameMeans(*frame);
        }
    }
    return std::nullopt;
}

}  // namespace sst::processing
