#pragma once

#include <cstdint>
#include <filesystem>

#include "domain/overlay/models/render-scene.hpp"

namespace sst::overlay {

// Captures the sequence of rendered overlay scenes during a recording and
// persists them next to the clean L1 MP4, so the overlay can be replayed and
// burned onto the recording on demand later (#6 F6b feeds F6c). The live
// pipeline records a CLEAN L1; this timeline is the only record of what the
// overlay showed, and when. Implemented by a filesystem adapter.
//
// Time base: scenes are stamped with the overlay's monotonic `NowMs` clock. The
// recording-start `anchor_ms` (also `NowMs`) lets a later replay map an L1 frame
// PTS back to overlay time as `anchor_ms + pts`, so the burned overlay lines up
// with the action within a frame.
class IOverlayTimelineRecorder {
   public:
    IOverlayTimelineRecorder() = default;
    virtual ~IOverlayTimelineRecorder() = default;
    IOverlayTimelineRecorder(const IOverlayTimelineRecorder&) = delete;
    auto operator=(const IOverlayTimelineRecorder&) -> IOverlayTimelineRecorder& = delete;
    IOverlayTimelineRecorder(IOverlayTimelineRecorder&&) = delete;
    auto operator=(IOverlayTimelineRecorder&&) -> IOverlayTimelineRecorder& = delete;

    // Begin capturing for the recording in `match_dir`, anchoring overlay time at
    // `anchor_ms` (the overlay NowMs clock value at recording start). Replaces any
    // in-progress capture.
    virtual auto Start(const std::filesystem::path& match_dir, std::uint64_t anchor_ms) -> void = 0;

    // Record one rendered scene at overlay-clock `at_ms`. No-op when not started.
    virtual auto OnScene(std::uint64_t at_ms, const RenderScene& scene) -> void = 0;

    // Flush the captured timeline to `<match_dir>/<matchId>.timeline.json` and
    // stop. No-op when not started or when nothing was captured.
    virtual auto Stop() -> void = 0;
};

}  // namespace sst::overlay
