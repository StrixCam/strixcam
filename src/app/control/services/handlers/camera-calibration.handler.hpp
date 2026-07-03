#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/color-calibration-state.hpp"

namespace sst::control {

// Handles SetCameraCalibrationCommand: writes the shared ColorCalibrationState
// the postprocessor samples each frame, so the app's diagnostic Calibration
// screen tunes the white-balance gains LIVE against the preview. Logs the applied
// values (so a tuned setting can be read from the console and baked as the saved
// default) and echoes them back for slider confirmation.
class CameraCalibrationHandler final : public ICommandHandler {
   public:
    explicit CameraCalibrationHandler(sst::processing::ColorCalibrationState& state);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::processing::ColorCalibrationState& state_;
};

}  // namespace sst::control
