#include "app/control/services/handlers/camera-focus.handler.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <vector>

namespace sst::control {

CameraFocusHandler::CameraFocusHandler(sst::focus::IFocuser& focuser, sst::focus::FocusState& state)
    : focuser_(focuser), state_(state) {}

auto CameraFocusHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kCameraFocus};
}

auto CameraFocusHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto& req = cmd.camera_focus();
    const bool is_auto = req.mode() == sst_cam::CameraFocusMode::FOCUS_MODE_AUTO;

    // camera_index absent => apply to both cameras.
    std::vector<std::uint32_t> cameras;
    if (req.has_camera_index()) {
        cameras.push_back(req.camera_index());
    } else {
        cameras = {0U, 1U};
    }

    for (const auto camera : cameras) {
        state_.SetAuto(camera, is_auto);
        // MANUAL with a position drives the VCM now + records it; a manual set
        // preempts any autofocus hunt (the AF loop skips non-auto cameras).
        if (!is_auto && req.has_focus_position()) {
            state_.SetManualPosition(camera, req.focus_position());
            focuser_.SetFocus(camera, req.focus_position());
        }
    }

    spdlog::info("Camera focus: mode={} cameras={} pos={} available={}",
                 is_auto ? "auto" : "manual", cameras.size(),
                 req.has_focus_position() ? static_cast<int>(req.focus_position()) : -1,
                 focuser_.IsAvailable());

    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_camera_focus();
    payload->set_mode(req.mode());
    if (req.has_focus_position()) {
        payload->set_focus_position(req.focus_position());
    }
    payload->set_autofocus_available(focuser_.IsAvailable());
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
