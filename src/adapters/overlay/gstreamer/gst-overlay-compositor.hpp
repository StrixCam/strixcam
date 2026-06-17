#pragma once

#include <gst/gst.h>

#include <cstdint>

#include "app/overlay/ports/overlay-sink.hpp"
#include "domain/common/models/output-size.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace sst::adapters::overlay {

// The overlay-source half of the composite-before-tee graph (U9, R20). Owns an
// `appsrc` that emits RGBA buffers; the pipeline orchestrator (U14) links this
// appsrc into a (nv)compositor sink pad layered over the live video, upstream of
// the tee, so the MP4 and RTSP/RTMP branches carry identical pixels.
//
// Push-on-change is the caller's policy (OverlayController): a new buffer is
// pushed only when the overlay changes; the compositor repeats the last buffer
// otherwise (zero per-frame overlay cost).
class GstOverlayCompositor final : public sst::overlay::IOverlaySink {
   public:
    explicit GstOverlayCompositor(sst::common::OutputSize size);
    ~GstOverlayCompositor() override;

    GstOverlayCompositor(const GstOverlayCompositor&) = delete;
    auto operator=(const GstOverlayCompositor&) -> GstOverlayCompositor& = delete;
    GstOverlayCompositor(GstOverlayCompositor&&) = delete;
    auto operator=(GstOverlayCompositor&&) -> GstOverlayCompositor& = delete;

    // Wrap the RGBA image in a GstBuffer and push it into the appsrc.
    auto PushFrame(const sst::overlay::RgbaImage& frame) -> void override;

    // The RGBA appsrc element, for the orchestrator to link into the compositor.
    [[nodiscard]] auto AppSrc() const -> GstElement* { return appsrc_; }

   private:
    std::uint32_t width_;
    std::uint32_t height_;
    GstElement* appsrc_{nullptr};
};

}  // namespace sst::adapters::overlay
