#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

#include "domain/capture/models/frame.hpp"

namespace sst::pipeline {

// On-request tap into a single camera's producer path (U6 autofocus). Unlike
// IFrameSnapshotSource (the post-processed CHOSEN frame), this hands out the
// materialized SOURCE frame of a specific camera — the autofocus loop needs
// per-camera luma even for the camera the decision did not select, and the
// always-on proxy can be legitimately stopped exactly in AF's main window
// (pre-match framing, nothing recording/streaming), so neither existing
// surface can serve it.
//
// Contract: GrabCameraFrame blocks the CALLER (never the producer) for up to
// `timeout` waiting for the next frame that camera produces, then returns a
// cheap descriptor copy sharing pixel ownership with the materialized frame —
// no per-frame pixel copy, and near-zero cost on the producer hot path when no
// request is pending (a single relaxed atomic load per frame). Returns
// std::nullopt on timeout, an out-of-range camera, or a stopped pipeline.
class ICameraFrameTap {
   public:
    virtual ~ICameraFrameTap() = default;

    [[nodiscard]] virtual auto GrabCameraFrame(std::size_t camera_index,
                                               std::chrono::milliseconds timeout)
        -> std::optional<sst::capture::Frame> = 0;
};

}  // namespace sst::pipeline
