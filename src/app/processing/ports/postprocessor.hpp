#pragma once

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
    // Returns std::nullopt and logs a warning on invalid input (unsupported
    // pixel format, out-of-bounds crop, zero geometry, unsupported output
    // format, etc).
    virtual auto Process(const sst::capture::Frame& source, const CropRect& crop)
        -> std::optional<sst::capture::Frame> = 0;
};

}  // namespace sst::processing
