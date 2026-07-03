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
    const sst::processing::ColorCalibrationState::Gains gains{req.r_gain(), req.g_gain(),
                                                              req.b_gain(), req.enabled()};
    state_.Set(gains);
    // Logged so a dialed-in setting can be read from the console + persisted as
    // the shipping default.
    spdlog::info("Camera WB calibration applied: enabled={} R={:.3f} G={:.3f} B={:.3f}",
                 gains.enabled, gains.r, gains.g, gains.b);

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
