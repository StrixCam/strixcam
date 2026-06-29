#pragma once

#include "domain/overlay/models/render-scene.hpp"

namespace sst::overlay {

// Destination for a freshly-rendered overlay frame. The GStreamer compositor
// adapter implements this by pushing the RGBA buffer into its appsrc; the
// compositor then holds that buffer between pushes (zero per-frame overlay cost).
// Pushed only when the overlay actually changes (push-on-change, R18/R20).
class IOverlaySink {
   public:
    virtual ~IOverlaySink() = default;

    virtual auto PushFrame(const RgbaImage& frame) -> void = 0;

    // Drop any retained overlay so the pipeline composites nothing until the next
    // PushFrame. Called when a match ends / no match is active so the live preview
    // stops showing a stale scoreboard. Default no-op for sinks that don't cache.
    virtual auto Clear() -> void {}
};

}  // namespace sst::overlay
