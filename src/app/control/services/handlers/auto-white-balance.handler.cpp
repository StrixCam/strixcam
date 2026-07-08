#include "app/control/services/handlers/auto-white-balance.handler.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace sst::control {

namespace {
// Match the app's slider range so the computed gains are always reachable/finite.
constexpr float kMinGain = 0.3F;
constexpr float kMaxGain = 1.7F;
// Below this channel mean the frame is too dark to trust a grey-world estimate.
constexpr float kMinUsableMean = 4.0F;

// Auto-tone targets. Brightness aims the post-WB luma at a slightly-below-mid
// level (headroom against blowout); contrast stretches a flat frame toward a
// target spread; saturation is a fixed mild boost (the ArduCAM renders
// consistently washed, and a content-derived saturation is unreliable).
constexpr float kTargetLuma = 118.0F;
constexpr float kTargetSpread = 52.0F;
constexpr float kAutoSaturation = 1.25F;
constexpr float kMinBrightness = -0.30F;
constexpr float kMaxBrightness = 0.30F;
constexpr float kMinContrast = 0.90F;
constexpr float kMaxContrast = 1.50F;

auto Clamp(float gain) -> float { return std::clamp(gain, kMinGain, kMaxGain); }
}  // namespace

AutoWhiteBalanceHandler::AutoWhiteBalanceHandler(
    sst::processing::FrameColorStats& stats, sst::processing::ColorCalibrationState& calibration,
    sst::processing::AutoColorState& auto_color)
    : stats_(stats), calibration_(calibration), auto_color_(auto_color) {}

auto AutoWhiteBalanceHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kAutoWhiteBalance};
}

auto AutoWhiteBalanceHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    const auto means = stats_.Get();

    // Grey-world normalized to green: gains that pull R and B onto G, so a neutral
    // target (white/grey) renders neutral. Green is the reference (gain 1.0) — never
    // boosted, which would tint the whole frame green. Guard a dark/no-sample frame.
    // Start from the current state so saturation/contrast/brightness are preserved
    // — auto-WB only touches the R/G/B gains.
    sst::processing::ColorCalibrationState::Gains gains = calibration_.Get();
    const bool usable = means.valid && means.g > kMinUsableMean && means.r > kMinUsableMean &&
                        means.b > kMinUsableMean;
    if (usable) {
        // White balance: grey-world normalized to green.
        gains.r = Clamp(means.g / means.r);
        gains.g = 1.0F;
        gains.b = Clamp(means.g / means.b);
        gains.enabled = true;

        // Saturation: fixed mild boost (the module renders washed).
        gains.saturation = kAutoSaturation;

        // Contrast: stretch a flat frame toward the target spread.
        gains.contrast = (means.spread > 1.0F)
                             ? std::clamp(kTargetSpread / means.spread, kMinContrast, kMaxContrast)
                             : 1.0F;

        // Brightness: aim the POST-WB luma (R/B are cut by the gains) at the
        // target. Additive fraction of full range.
        const float post_luma =
            (0.299F * gains.r * means.r) + (0.587F * means.g) + (0.114F * gains.b * means.b);
        gains.brightness =
            // NOLINTNEXTLINE(readability-magic-numbers) — 255 = 8-bit channel max
            std::clamp((kTargetLuma - post_luma) / 255.0F, kMinBrightness, kMaxBrightness);

        calibration_.Set(gains);
        // A one-shot result is a user override: hold it — pause the continuous
        // auto-WB loop (SetCameraCalibration enabled=false resumes it).
        auto_color_.SetMode(sst::processing::AutoColorMode::kManual);
        spdlog::info(
            "Auto color: means B={:.1f} G={:.1f} R={:.1f} spread={:.1f} -> R={:.3f} G={:.3f} "
            "B={:.3f} sat={:.2f} con={:.2f} bri={:.2f}",
            means.b, means.g, means.r, means.spread, gains.r, gains.g, gains.b, gains.saturation,
            gains.contrast, gains.brightness);
    } else {
        spdlog::warn("Auto color: frame too dark / no sample — keeping current values");
    }

    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_camera_calibration();
    payload->set_r_gain(gains.r);
    payload->set_g_gain(gains.g);
    payload->set_b_gain(gains.b);
    payload->set_enabled(gains.enabled);
    payload->set_saturation(gains.saturation);
    payload->set_contrast(gains.contrast);
    payload->set_brightness(gains.brightness);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
