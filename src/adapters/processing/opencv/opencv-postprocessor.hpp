#pragma once

#include <memory>
#include <optional>

#include "app/processing/ports/postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/frame-color-stats.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::adapters::processing {

class VicConvertStage;

class OpenCvPostprocessor final : public sst::processing::IPostprocessor {
   public:
    // `calibration`, when non-null, supplies the WB gains LIVE each frame (driven
    // by the diagnostic calibration screen) and overrides config.color_correction.
    // `frame_stats`, when non-null, receives the PRE-correction BGR average each
    // frame — the auto-white-balance handler reads it to compute grey-world gains.
    // Null args => the static config gains / no stats (tests, or no tuning wired).
    //
    // The NV12->BGR convert + scale (the biggest per-frame CPU cost feeding
    // every encode branch) is delegated to an internal VIC stage
    // (VicConvertStage) when the hardware is present; OpenCV keeps only the
    // calibration math. SST_DISABLE_VIC=1 (read at construction) — or a missing
    // nvvidconv, e.g. the dev container — falls back to the full-OpenCV path
    // with identical calibration behaviour.
    explicit OpenCvPostprocessor(
        sst::processing::PostprocessConfig config = {},
        const sst::processing::ColorCalibrationState* calibration = nullptr,
        sst::processing::FrameColorStats* frame_stats = nullptr);
    ~OpenCvPostprocessor() override;

    OpenCvPostprocessor(const OpenCvPostprocessor&) = delete;
    auto operator=(const OpenCvPostprocessor&) -> OpenCvPostprocessor& = delete;
    OpenCvPostprocessor(OpenCvPostprocessor&&) = delete;
    auto operator=(OpenCvPostprocessor&&) -> OpenCvPostprocessor& = delete;

    auto Process(const sst::capture::Frame& source, const sst::processing::CropRect& crop)
        -> std::optional<sst::capture::Frame> override;

   private:
    sst::processing::PostprocessConfig config_;
    const sst::processing::ColorCalibrationState* calibration_;
    sst::processing::FrameColorStats* frame_stats_;
    // Present unless SST_DISABLE_VIC; internally capability-gated (no-op off
    // Jetson). unique_ptr keeps the GStreamer types out of this header.
    std::unique_ptr<VicConvertStage> vic_;
};

}  // namespace sst::adapters::processing
