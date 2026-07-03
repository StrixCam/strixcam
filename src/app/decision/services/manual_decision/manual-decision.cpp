#include "app/decision/services/manual_decision/manual-decision.hpp"

#include <array>
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
    // Preference: the selected camera, then the other(s) as a live fallback so a
    // momentary missing frame on the chosen camera doesn't black out the output.
    const std::array<std::uint32_t, 3> order{want, 0U, 1U};
    for (const std::uint32_t idx : order) {
        if (idx >= cameras.size()) {
            continue;
        }
        const auto& slot = cameras[idx];
        if (!slot.has_value()) {
            continue;
        }
        const auto& geometry = slot->source_frame.geometry;
        if (geometry.width == 0 || geometry.height == 0) {
            continue;
        }
        return CameraChoice{
            .camera_index = idx,
            .crop = sst::processing::CropRect{
                .x = 0, .y = 0, .width = geometry.width, .height = geometry.height},
        };
    }
    return std::nullopt;
}

}  // namespace sst::decision
