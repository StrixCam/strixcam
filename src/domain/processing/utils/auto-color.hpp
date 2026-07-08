#pragma once

#include <optional>
#include <span>

#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/color-calibration-state.hpp"

namespace sst::processing {

// Pure math for the continuous auto white-balance loop (AutoColorService):
// per-camera channel means -> one damped grey-world step toward a SHARED
// neutral target. No I/O, no threads — fully unit-testable.

// Average R/G/B of a sampled frame (pre-correction — the raw sensor cast).
struct ChannelMeans {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};

    [[nodiscard]] auto Luma() const -> float { return (r + g + b) / kChannelCount; }

   private:
    static constexpr float kChannelCount = 3.0F;
};

// Step tuning. Defaults give: dead-band so a settled scene never pumps, and
// ~90% convergence in ~8 applied steps (alpha 0.25) — ~25 s at the service's
// few-second tick. Gains clamp to the app slider range so a manual reading of
// the applied values is always representable.
struct AutoColorTuning {
    // Damping: fraction of the remaining error applied per step.
    float alpha{kDefaultAlpha};
    // No write while every channel's ideal gain is this close to the current.
    float dead_band{kDefaultDeadBand};
    float min_gain{kDefaultMinGain};  // slider floor
    float max_gain{kDefaultMaxGain};  // slider ceiling
    // Below this a channel is too dark to trust a grey-world estimate.
    float min_usable_mean{kDefaultMinUsableMean};

   private:
    static constexpr float kDefaultAlpha = 0.25F;
    static constexpr float kDefaultDeadBand = 0.02F;
    static constexpr float kDefaultMinGain = 0.3F;
    static constexpr float kDefaultMaxGain = 1.7F;
    static constexpr float kDefaultMinUsableMean = 4.0F;
};

// Channel means of an NV12 CPU frame (subsampled — a statistics pass, not a
// conversion). Uses the BT.601 full-range YUV->RGB transform (the same family
// the postprocessor's NV12->BGR convert applies), and the transform is linear,
// so plane means convert directly. nullopt for non-NV12 / non-CPU / degenerate
// frames.
auto MeasureFrameMeans(const sst::capture::Frame& frame) -> std::optional<ChannelMeans>;

// The SHARED neutral target grey level: the average luma of every camera that
// produced a usable sample this tick. Both cameras chasing one target is what
// matches them — per-camera targets would only make each camera self-neutral.
// nullopt when no camera sampled usably (target holds, no step this tick).
auto SharedNeutralTarget(std::span<const std::optional<ChannelMeans>> means)
    -> std::optional<float>;

// One damped grey-world step for one camera: pull the R/G/B gains a fraction
// (alpha) of the way toward target/mean per channel. Returns the next gains to
// apply, or nullopt when no write is needed/safe:
//   - any channel mean below min_usable_mean (too dark to trust), or
//   - every channel's ideal gain is within dead_band of the current gain
//     (hysteresis — a converged loop writes nothing and cannot pump).
// Saturation/contrast/brightness pass through untouched (auto-WB owns only
// the WB gains); the returned gains are enabled.
auto GreyWorldStep(const ColorCalibrationState::Gains& current, const ChannelMeans& means,
                   float target,
                   const AutoColorTuning& tuning) -> std::optional<ColorCalibrationState::Gains>;

}  // namespace sst::processing
