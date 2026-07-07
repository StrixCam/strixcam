#pragma once

#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::bootstrap {

// WB magenta-correction gains are env-tunable so the cast can be dialed in
// on-device without a rebuild (a fixed grey-world gain over/under-shoots per
// scene). SST_WB_DISABLE turns it off; SST_WB_{R,G,B}GAIN override each gain.
// Logs the resolved gains.
auto ResolvePostprocessConfig() -> sst::processing::PostprocessConfig;

// Initial live WB calibration (diagnostic Calibration screen), seeded from the
// resolved default/env gains plus the SST_{SATURATION,CONTRAST,BRIGHTNESS}
// overrides. The postprocessor samples the resulting state each frame and the
// SetCameraCalibration handler writes it, so slider drags retune the preview
// live.
auto InitialCalibrationGains(const sst::processing::PostprocessConfig& config)
    -> sst::processing::ColorCalibrationState::Gains;

}  // namespace sst::bootstrap
