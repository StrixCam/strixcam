#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/auto-color-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"

namespace sst::control {

// Handles AutoWhiteBalanceCommand: reads the postprocessor's latest pre-correction
// frame average (FrameColorStats), computes grey-world gains that neutralize it
// (normalize to green so green is never boosted — a G boost tints the frame), and
// writes them into the shared ColorCalibrationState (every camera) so the
// correction takes effect live. A successful one-shot is a USER OVERRIDE: it
// flips AutoColorMode to kManual so the continuous auto-WB loop holds these
// values instead of drifting them (SetCameraCalibration enabled=false resumes
// the loop). Replies with a CameraCalibrationResponse carrying the computed
// gains so the app can seed its sliders. Point the camera at a white/grey
// surface first.
class AutoWhiteBalanceHandler final : public ICommandHandler {
   public:
    AutoWhiteBalanceHandler(sst::processing::FrameColorStats& stats,
                            sst::processing::ColorCalibrationState& calibration,
                            sst::processing::AutoColorState& auto_color);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::processing::FrameColorStats& stats_;
    sst::processing::ColorCalibrationState& calibration_;
    sst::processing::AutoColorState& auto_color_;
};

}  // namespace sst::control
