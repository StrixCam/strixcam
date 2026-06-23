#pragma once

#include <cstdint>

#include "domain/overlay/models/render-scene.hpp"

namespace sst::overlay {

// Rasterizes a resolved scene to an RGBA surface. The scene is in logical
// canvas coordinates; the renderer scales to the requested output resolution.
// Pure rendering — no bindings, no templates (the scene service resolves those).
class IOverlayRenderer {
   public:
    virtual ~IOverlayRenderer() = default;

    virtual auto Render(const RenderScene& scene, std::uint32_t out_width,
                        std::uint32_t out_height) -> RgbaImage = 0;
};

}  // namespace sst::overlay
