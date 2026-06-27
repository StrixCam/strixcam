#pragma once

#include <filesystem>

#include "domain/overlay/models/overlay-timeline.hpp"

namespace sst::overlay {

// Burns an overlay timeline onto a clean L1 recording, producing an overlaid L2
// MP4 (#6 F6c). Decodes L1 frame by frame, composites the timeline scene active
// at each frame's overlay time (`timeline.anchor_ms + frame_pts`), and encodes
// the result. Offline / CPU-bound — runs in a background export job, never
// during a live session (the no-NVENC Orin Nano can't spare the encode).
class IOverlayBurner {
   public:
    IOverlayBurner() = default;
    virtual ~IOverlayBurner() = default;
    IOverlayBurner(const IOverlayBurner&) = delete;
    auto operator=(const IOverlayBurner&) -> IOverlayBurner& = delete;
    IOverlayBurner(IOverlayBurner&&) = delete;
    auto operator=(IOverlayBurner&&) -> IOverlayBurner& = delete;

    // Decode `l1_path`, composite `timeline`, write the overlaid result to
    // `l2_path`. Returns true on success. On failure, `l2_path` must not be left
    // as a partial/corrupt file (the implementation removes it).
    [[nodiscard]] virtual auto Burn(const std::filesystem::path& l1_path,
                                    const OverlayTimeline& timeline,
                                    const std::filesystem::path& l2_path) -> bool = 0;
};

}  // namespace sst::overlay
