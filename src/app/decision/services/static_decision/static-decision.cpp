#include "app/decision/services/static_decision/static-decision.hpp"

#include <spdlog/spdlog.h>

#include "domain/processing/models/crop-rect.hpp"

namespace sst::decision {

namespace {
constexpr std::uint32_t kCamera0Index = 0;
}  // namespace

auto StaticDecision::Decide(const std::vector<std::optional<sst::processing::FrameBundle>>& cameras)
    -> std::optional<CameraChoice> {
    if (cameras.empty() || !cameras[kCamera0Index].has_value()) {
        // Camera 0 produced nothing this tick — nothing to present. The
        // consumer skips and retries; camera 1 (if any) is intentionally
        // ignored by the static policy.
        return std::nullopt;
    }

    const auto& geometry = cameras[kCamera0Index]->source_frame.geometry;
    if (geometry.width == 0 || geometry.height == 0) {
        spdlog::warn("StaticDecision: camera 0 has degenerate geometry {}x{}; skipping tick",
                     geometry.width, geometry.height);
        return std::nullopt;
    }

    return CameraChoice{
        .camera_index = kCamera0Index,
        .crop =
            sst::processing::CropRect{
                .x = 0, .y = 0, .width = geometry.width, .height = geometry.height},
    };
}

}  // namespace sst::decision
