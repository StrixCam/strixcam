#pragma once

#include <optional>

#include "domain/capture/models/frame.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace sst::overlay {

// Alpha-blend an RGBA8888 overlay onto a BGR8 source frame, returning a NEW
// owned BGR8 frame (the source is never mutated — its planes are const). The
// overlay is uniformly scaled (aspect-preserving) and centered over the source
// (letterboxed), matching the overlay-rendering.md "uniform scale + letterbox"
// rule; the letterbox margins are left as the untouched source video.
//
// Returns std::nullopt when there is nothing to composite or the inputs are
// unsupported (source not BGR8, empty geometry, empty overlay) — the caller
// then passes the original frame through unchanged. Fully-transparent overlays
// blend to a visually clean frame, so a "disabled" overlay (empty scene → all
// alpha 0) needs no special-casing upstream.
auto CompositeOverlay(const sst::capture::Frame& source, const RgbaImage& overlay)
    -> std::optional<sst::capture::Frame>;

}  // namespace sst::overlay
