#include "adapters/overlay/timeline/overlay-timeline-loader.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "adapters/overlay/timeline/serde/render-scene-serde.hpp"

namespace sst::adapters::overlay {

auto LoadOverlayTimeline(const std::filesystem::path& path)
    -> std::optional<sst::overlay::OverlayTimeline> {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    try {
        const auto doc = nlohmann::json::parse(file);
        sst::overlay::OverlayTimeline timeline;
        timeline.anchor_ms = doc.at("anchor_ms").get<std::uint64_t>();
        for (const auto& event : doc.at("events")) {
            sst::overlay::TimelineEvent parsed;
            parsed.at_ms = event.at("at_ms").get<std::uint64_t>();
            event.at("scene").get_to(parsed.scene);
            timeline.events.push_back(std::move(parsed));
        }
        return timeline;
    } catch (const nlohmann::json::exception& ex) {
        spdlog::warn("OverlayTimeline: malformed {}: {}", path.string(), ex.what());
        return std::nullopt;
    }
}

}  // namespace sst::adapters::overlay
