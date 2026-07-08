#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/auto-color-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"

namespace sst::control {

// Handles SetCameraCalibrationCommand: writes the shared ColorCalibrationState
// (every camera — one slider set applies rig-wide) the postprocessor samples
// each frame, so the app's diagnostic Calibration screen tunes the
// white-balance gains LIVE against the preview. A calibration with
// enabled=true is a USER OVERRIDE: it flips AutoColorMode to kManual so the
// continuous auto-WB loop stands down instead of fighting the sliders;
// enabled=false hands authority back to the loop (kAuto). Logs the applied
// values and echoes them back for slider confirmation.
class CameraCalibrationHandler final : public ICommandHandler {
   public:
    CameraCalibrationHandler(sst::processing::ColorCalibrationState& state,
                             sst::processing::AutoColorState& auto_color);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::processing::ColorCalibrationState& state_;
    sst::processing::AutoColorState& auto_color_;
};

}  // namespace sst::control
