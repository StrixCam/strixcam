#pragma once

#include <optional>

#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace sst::processing {

class IPreprocessor {
   public:
    IPreprocessor() = default;
    virtual ~IPreprocessor() = default;

    IPreprocessor(const IPreprocessor&) = delete;
    auto operator=(const IPreprocessor&) -> IPreprocessor& = delete;
    IPreprocessor(IPreprocessor&&) = delete;
    auto operator=(IPreprocessor&&) -> IPreprocessor& = delete;

    // Transform a raw captured Frame into a FrameBundle: an AI-ready small
    // frame plus a value-copy of the source. Returns std::nullopt and logs a
    // warning on invalid input (unsupported pixel format, zero geometry, odd
    // NV12 dimensions, etc).
    virtual auto Process(const sst::capture::Frame& raw) -> std::optional<FrameBundle> = 0;
};

}  // namespace sst::processing
