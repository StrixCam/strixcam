#pragma once

#include <cstddef>
#include <optional>

#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/crop-rect.hpp"

namespace sst::processing {

class IPostprocessor {
   public:
    IPostprocessor() = default;
    virtual ~IPostprocessor() = default;

    IPostprocessor(const IPostprocessor&) = delete;
    auto operator=(const IPostprocessor&) -> IPostprocessor& = delete;
    IPostprocessor(IPostprocessor&&) = delete;
    auto operator=(IPostprocessor&&) -> IPostprocessor& = delete;

    // Crop a region of `source` and resize it to the configured output
    // resolution and format. `crop` is in source-frame pixel coordinates.
    // `camera_index` identifies the camera that produced `source` — the color
    // calibration applied is PER CAMERA (the continuous auto-WB loop converges
    // each module's own gains toward the shared neutral target). Returns
    // std::nullopt and logs a warning on invalid input (unsupported pixel
    // format, out-of-bounds crop, zero geometry, unsupported output format,
    // etc).
    virtual auto Process(const sst::capture::Frame& source, const CropRect& crop,
                         std::size_t camera_index) -> std::optional<sst::capture::Frame> = 0;
};

}  // namespace sst::processing
