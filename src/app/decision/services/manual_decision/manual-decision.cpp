#include "app/decision/services/manual_decision/manual-decision.hpp"

#include <cstdint>

#include "domain/processing/models/crop-rect.hpp"

namespace sst::decision {

ManualDecision::ManualDecision(const ManualCameraState& state) : state_(state) {}

auto ManualDecision::Decide(
    const std::vector<std::optional<sst::processing::FrameBundle>>& cameras)
    -> std::optional<CameraChoice> {
    if (cameras.empty()) {
        return std::nullopt;
    }
    const std::uint32_t want = state_.Get();
    // Honor the explicit selection. An out-of-range selection (misconfiguration)
    // falls back to camera 0 once so the device still shows something rather than
    // a permanent black screen — but a VALID selection is never swapped for
    // another camera on a per-tick miss (see below).
    const std::uint32_t idx = (want < cameras.size()) ? want : 0U;
    const auto& slot = cameras[idx];
    // Selected camera has no frame THIS tick: hold — return nullopt so the
    // consumer skips the push and every sink keeps its last frame — instead of
    // cross-switching to the other camera. The cadence camera (camera 0 today) is
    // blocking-popped every tick and is always present, but a non-cadence
    // selected camera is only TryPop'd, so its slot is intermittently empty on a
    // phase miss. Cross-switching to camera 0 on that miss is the visible 0<->1
    // flicker, and it also splices foreign-camera frames into a recording tagged
    // for the selected camera. A held last frame reads as smooth; a camera swap
    // does not. (The deeper fix — make the cadence camera follow the selection so
    // it is never phase-empty — is tracked separately.)
    if (!slot.has_value()) {
        return std::nullopt;
    }
    const auto& geometry = slot->source_frame.geometry;
    if (geometry.width == 0 || geometry.height == 0) {
        return std::nullopt;
    }
    return CameraChoice{
        .camera_index = idx,
        .crop = sst::processing::CropRect{
            .x = 0, .y = 0, .width = geometry.width, .height = geometry.height},
    };
}

}  // namespace sst::decision
