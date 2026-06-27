#pragma once

#include <optional>

#include "domain/capture/models/frame.hpp"

namespace sst::processing {

// Composites two post-processed BGR frames into one output frame of a fixed
// configured geometry — the side-by-side dual-camera monitoring view (#6 F6d).
// Each input is letterboxed (aspect preserved) into one half of the canvas, so
// the output matches the single-stream geometry the RTSP encoder already
// expects (no caps renegotiation). Returns std::nullopt on invalid input
// (wrong pixel format, zero geometry).
class IFrameCompositor {
   public:
    IFrameCompositor() = default;
    virtual ~IFrameCompositor() = default;

    IFrameCompositor(const IFrameCompositor&) = delete;
    auto operator=(const IFrameCompositor&) -> IFrameCompositor& = delete;
    IFrameCompositor(IFrameCompositor&&) = delete;
    auto operator=(IFrameCompositor&&) -> IFrameCompositor& = delete;

    virtual auto CompositeSideBySide(const sst::capture::Frame& left,
                                     const sst::capture::Frame& right)
        -> std::optional<sst::capture::Frame> = 0;
};

}  // namespace sst::processing
