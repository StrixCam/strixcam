#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/focus/ports/focuser.hpp"
#include "bluetooth.pb.h"
#include "domain/focus/models/focus-state.hpp"

namespace sst::control {

// Handles CameraFocusControlCommand. MANUAL + focus_position drives the VCM
// immediately (and records the position); AUTO hands the camera to the autofocus
// loop by flipping FocusState. camera_index absent => both cameras. The reply
// carries autofocus_available (the focuser's I2C buses opened) — false means a
// fixed lens, and the command still succeeds (never fails), per the contract.
class CameraFocusHandler final : public ICommandHandler {
   public:
    CameraFocusHandler(sst::focus::IFocuser& focuser, sst::focus::FocusState& state);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::focus::IFocuser& focuser_;
    sst::focus::FocusState& state_;
};

}  // namespace sst::control
