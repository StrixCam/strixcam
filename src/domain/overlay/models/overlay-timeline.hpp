#pragma once

#include <cstdint>
#include <vector>

#include "domain/overlay/models/render-scene.hpp"

namespace sst::overlay {

// One captured overlay state: the resolved scene shown from `at_ms` (overlay
// monotonic clock) until the next event.
struct TimelineEvent {
    std::uint64_t at_ms{0};
    RenderScene scene;
};

// The full overlay timeline for one recording (persisted by F6b, replayed by
// F6c). `anchor_ms` is the overlay-clock value at recording start, so an L1
// frame at presentation time `pts_ms` maps to overlay time `anchor_ms + pts_ms`.
// `events` are in ascending `at_ms` order.
struct OverlayTimeline {
    std::uint64_t anchor_ms{0};
    std::vector<TimelineEvent> events;
};

// The scene visible at overlay-clock time `overlay_ms` — the last event whose
// `at_ms <= overlay_ms` (the overlay holds the last pushed scene until it
// changes). Returns nullptr before the first event (no overlay yet). The pointer
// is valid for the lifetime of `timeline`.
[[nodiscard]] inline auto SceneAtOverlayTime(const OverlayTimeline& timeline,
                                             std::uint64_t overlay_ms) -> const RenderScene* {
    const RenderScene* current = nullptr;
    for (const auto& event : timeline.events) {
        if (event.at_ms > overlay_ms) {
            break;  // events are ascending — nothing later applies
        }
        current = &event.scene;
    }
    return current;
}

}  // namespace sst::overlay
