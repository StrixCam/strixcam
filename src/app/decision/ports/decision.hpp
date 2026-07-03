#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "domain/decision/models/camera-choice.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace sst::decision {

// The intelligence seam. Given the latest materialized bundle from each camera
// (indexed by camera_index; std::nullopt means that camera has no fresh frame
// this tick), choose which camera to present and the crop to apply.
//
// Returns std::nullopt when no usable frame is available — the caller skips the
// tick rather than postprocessing nothing. Implementations must be deterministic
// and must not pin the passed bundles past the call (the orchestrator owns them).
//
// `StaticDecision` is the no-AI implementation (always camera 0, full frame).
// The AI/physics/decision stack later implements the same port with dynamic
// camera selection and zoom/crop.
class IDecision {
   public:
    IDecision() = default;
    virtual ~IDecision() = default;

    IDecision(const IDecision&) = delete;
    auto operator=(const IDecision&) -> IDecision& = delete;
    IDecision(IDecision&&) = delete;
    auto operator=(IDecision&&) -> IDecision& = delete;

    virtual auto Decide(const std::vector<std::optional<sst::processing::FrameBundle>>& cameras)
        -> std::optional<CameraChoice> = 0;

    // The camera the consumer should BLOCK on to pace its loop each tick (the
    // "cadence" camera) — the one this decision most wants a guaranteed-fresh
    // frame from. Every other camera is sampled non-blocking (a stale/missing
    // frame there is tolerable). Defaults to camera 0; ManualDecision returns the
    // user-selected camera so the blocking pop follows the selection and the
    // chosen camera is never phase-empty (a non-cadence selection was what made
    // the preview flicker). The orchestrator clamps an out-of-range value to 0.
    [[nodiscard]] virtual auto PreferredCadenceCamera() const -> std::size_t { return 0; }
};

}  // namespace sst::decision
