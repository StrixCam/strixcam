#include "app/control/services/handlers/active-camera.handler.hpp"

#include <spdlog/spdlog.h>

namespace sst::control {

ActiveCameraHandler::ActiveCameraHandler(sst::decision::ManualCameraState& state) : state_(state) {}

auto ActiveCameraHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetActiveCamera};
}

auto ActiveCameraHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto camera_index = cmd.set_active_camera().camera_index();
    state_.Set(camera_index);
    spdlog::info("Active camera (manual tracking) set to {}", camera_index);

    sst_cam::CommandResponse resp;
    resp.mutable_active_camera()->set_camera_index(camera_index);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
