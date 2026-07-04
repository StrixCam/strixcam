#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"

namespace sst::control {

// Handles AutoWhiteBalanceCommand: reads the postprocessor's latest pre-correction
// frame average (FrameColorStats), computes grey-world gains that neutralize it
// (normalize to green so green is never boosted — a G boost tints the frame), and
// writes them into the shared ColorCalibrationState so the correction takes effect
// live. Replies with a CameraCalibrationResponse carrying the computed gains so the
// app can seed its sliders. Point the camera at a white/grey surface first.
class AutoWhiteBalanceHandler final : public ICommandHandler {
   public:
    AutoWhiteBalanceHandler(sst::processing::FrameColorStats& stats,
                            sst::processing::ColorCalibrationState& calibration);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::processing::FrameColorStats& stats_;
    sst::processing::ColorCalibrationState& calibration_;
};

}  // namespace sst::control
