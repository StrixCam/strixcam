#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/decision/models/manual-camera-state.hpp"

namespace sst::control {

// Handles SetActiveCameraCommand: writes the manually-selected output camera into
// ManualCameraState, which ManualDecision reads each tick to pick the camera fed
// to record/stream/single-preview. The human override of the future AI decision.
class ActiveCameraHandler final : public ICommandHandler {
   public:
    explicit ActiveCameraHandler(sst::decision::ManualCameraState& state);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::decision::ManualCameraState& state_;
};

}  // namespace sst::control
