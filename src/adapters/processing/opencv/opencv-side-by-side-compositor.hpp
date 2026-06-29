#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>

#include "app/processing/ports/frame-compositor.hpp"
#include "domain/capture/models/frame.hpp"

namespace sst::adapters::processing {

// OpenCV side-by-side compositor (#6 F6d). Letterboxes two BGR8 frames into the
// left and right halves of a fixed output canvas, preserving each camera's
// aspect ratio (black bars fill the remainder). The output canvas matches the
// preview stream geometry so the RTSP encoder caps never change.
class OpenCvSideBySideCompositor final : public sst::processing::IFrameCompositor {
   public:
    OpenCvSideBySideCompositor(std::uint32_t output_width, std::uint32_t output_height);

    auto CompositeSideBySide(const sst::capture::Frame& left, const sst::capture::Frame& right)
        -> std::optional<sst::capture::Frame> override;

   private:
    std::uint32_t output_width_;
    std::uint32_t output_height_;
    // Reused output canvas — the compositor runs once per live frame on the
    // single consumer thread, so a member buffer avoids re-allocating the full
    // canvas (and the per-column temporaries) every frame. Not thread-safe by
    // design: only the pipeline consumer loop calls CompositeSideBySide.
    cv::Mat canvas_;
};

}  // namespace sst::adapters::processing
