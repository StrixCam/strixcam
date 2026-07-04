#pragma once

#include <optional>

#include "app/processing/ports/postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/frame-color-stats.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::adapters::processing {

class OpenCvPostprocessor final : public sst::processing::IPostprocessor {
   public:
    // `calibration`, when non-null, supplies the WB gains LIVE each frame (driven
    // by the diagnostic calibration screen) and overrides config.color_correction.
    // `frame_stats`, when non-null, receives the PRE-correction BGR average each
    // frame — the auto-white-balance handler reads it to compute grey-world gains.
    // Null args => the static config gains / no stats (tests, or no tuning wired).
    explicit OpenCvPostprocessor(
        sst::processing::PostprocessConfig config = {},
        const sst::processing::ColorCalibrationState* calibration = nullptr,
        sst::processing::FrameColorStats* frame_stats = nullptr);
    ~OpenCvPostprocessor() override = default;

    auto Process(const sst::capture::Frame& source, const sst::processing::CropRect& crop)
        -> std::optional<sst::capture::Frame> override;

   private:
    sst::processing::PostprocessConfig config_;
    const sst::processing::ColorCalibrationState* calibration_;
    sst::processing::FrameColorStats* frame_stats_;
};

}  // namespace sst::adapters::processing
