#pragma once

#include <optional>

#include "app/processing/ports/postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::adapters::processing {

class OpenCvPostprocessor final : public sst::processing::IPostprocessor {
   public:
    // `calibration`, when non-null, supplies the WB gains LIVE each frame (driven
    // by the diagnostic calibration screen) and overrides config.color_correction.
    // Null => the static config gains (tests, or no live-tuning wired).
    explicit OpenCvPostprocessor(sst::processing::PostprocessConfig config = {},
                                 const sst::processing::ColorCalibrationState* calibration = nullptr);
    ~OpenCvPostprocessor() override = default;

    auto Process(const sst::capture::Frame& source, const sst::processing::CropRect& crop)
        -> std::optional<sst::capture::Frame> override;

   private:
    sst::processing::PostprocessConfig config_;
    const sst::processing::ColorCalibrationState* calibration_;
};

}  // namespace sst::adapters::processing
