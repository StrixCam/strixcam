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

auto Clamp(float gain) -> float { return std::clamp(gain, kMinGain, kMaxGain); }
}  // namespace

AutoWhiteBalanceHandler::AutoWhiteBalanceHandler(
    sst::processing::FrameColorStats& stats, sst::processing::ColorCalibrationState& calibration)
    : stats_(stats), calibration_(calibration) {}

auto AutoWhiteBalanceHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kAutoWhiteBalance};
}

auto AutoWhiteBalanceHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    const auto means = stats_.Get();

    // Grey-world normalized to green: gains that pull R and B onto G, so a neutral
    // target (white/grey) renders neutral. Green is the reference (gain 1.0) — never
    // boosted, which would tint the whole frame green. Guard a dark/no-sample frame.
    sst::processing::ColorCalibrationState::Gains gains = calibration_.Get();  // fall back to current
    const bool usable = means.valid && means.g > kMinUsableMean && means.r > kMinUsableMean &&
                        means.b > kMinUsableMean;
    if (usable) {
        gains = {Clamp(means.g / means.r), 1.0F, Clamp(means.g / means.b), true};
        calibration_.Set(gains);
        spdlog::info(
            "Auto WB: frame means B={:.1f} G={:.1f} R={:.1f} -> gains R={:.3f} G={:.3f} B={:.3f}",
            means.b, means.g, means.r, gains.r, gains.g, gains.b);
    } else {
        spdlog::warn("Auto WB: frame too dark / no sample — keeping current gains");
    }

    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_camera_calibration();
    payload->set_r_gain(gains.r);
    payload->set_g_gain(gains.g);
    payload->set_b_gain(gains.b);
    payload->set_enabled(gains.enabled);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
