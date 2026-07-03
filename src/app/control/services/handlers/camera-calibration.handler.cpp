#include "app/control/services/handlers/camera-calibration.handler.hpp"

#include <spdlog/spdlog.h>

namespace sst::control {

CameraCalibrationHandler::CameraCalibrationHandler(sst::processing::ColorCalibrationState& state)
    : state_(state) {}

auto CameraCalibrationHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetCameraCalibration};
}

auto CameraCalibrationHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto& req = cmd.set_camera_calibration();
    // proto3 scalars default to 0, but the identity for saturation/contrast is 1.0
    // (0 would grey/black the frame). Treat a non-positive value as "unset → 1.0"
    // so an older/partial command can't blank the image. brightness 0 IS identity.
    const sst::processing::ColorCalibrationState::Gains gains{
        .r = req.r_gain(),
        .g = req.g_gain(),
        .b = req.b_gain(),
        .enabled = req.enabled(),
        .saturation = req.saturation() > 0.0F ? req.saturation() : 1.0F,
        .contrast = req.contrast() > 0.0F ? req.contrast() : 1.0F,
        .brightness = req.brightness()};
    state_.Set(gains);
    // Logged so a dialed-in setting can be read from the console + persisted as
    // the shipping default.
    spdlog::info(
        "Camera calibration applied: enabled={} R={:.3f} G={:.3f} B={:.3f} sat={:.2f} "
        "con={:.2f} bri={:.2f}",
        gains.enabled, gains.r, gains.g, gains.b, gains.saturation, gains.contrast,
        gains.brightness);

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
