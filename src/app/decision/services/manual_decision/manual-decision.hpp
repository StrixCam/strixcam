#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "app/decision/ports/decision.hpp"
#include "domain/decision/models/camera-choice.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace sst::decision {

// Manual-override decision: presents the camera the user selected
// (ManualCameraState), full frame. If the selected camera has no fresh frame this
// tick it returns nullopt (the consumer holds the last frame) rather than
// cross-switching to another camera — a per-tick miss must not yank the output to
// a different camera (that swap is the preview flicker). PreferredCadenceCamera()
// makes the consumer block on the selected camera so those misses are rare. The
// manual stand-in for the eventual AI decision — same IDecision port; the AI
// later supplies dynamic selection + crop, with this as the human override.
class ManualDecision final : public IDecision {
   public:
    explicit ManualDecision(const ManualCameraState& state);

    auto Decide(const std::vector<std::optional<sst::processing::FrameBundle>>& cameras)
        -> std::optional<CameraChoice> override;

    [[nodiscard]] auto PreferredCadenceCamera() const -> std::size_t override;

   private:
    const ManualCameraState& state_;
};

}  // namespace sst::decision
