#pragma once

#include <atomic>
#include <filesystem>

#include "adapters/overlay/cairo/cairo-overlay-renderer.hpp"
#include "app/overlay/ports/overlay-burner.hpp"
#include "domain/overlay/models/overlay-timeline.hpp"

namespace sst::adapters::overlay {

// OpenCV (ffmpeg-backed) implementation of the overlay burn (#6 F6c). Decodes
// the clean L1 MP4 with cv::VideoCapture, composites the timeline scene active
// at each frame's overlay time, and re-encodes the overlaid result with
// cv::VideoWriter (H.264). CPU-only — the Orin Nano has no NVENC; this runs in a
// background export job, never alongside a live encode. Owns its own Cairo
// renderer so it never contends with the live overlay path.
class OpenCvOverlayBurner final : public sst::overlay::IOverlayBurner {
   public:
    [[nodiscard]] auto Burn(const std::filesystem::path& l1_path,
                            const sst::overlay::OverlayTimeline& timeline,
                            const std::filesystem::path& l2_path,
                            const std::atomic<bool>& cancel) -> bool override;

   private:
    CairoOverlayRenderer renderer_;
};

}  // namespace sst::adapters::overlay
