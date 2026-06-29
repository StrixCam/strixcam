#pragma once

#include <filesystem>
#include <optional>

#include "domain/overlay/models/overlay-timeline.hpp"

namespace sst::adapters::overlay {

// Loads an overlay timeline written by FilesystemOverlayTimelineRecorder (#6
// F6b) from `<matchId>.timeline.json`. Returns nullopt if the file is missing or
// malformed (a recording made before overlay capture, or a corrupt file) — the
// caller treats that as "no overlay to burn".
[[nodiscard]] auto LoadOverlayTimeline(const std::filesystem::path& path)
    -> std::optional<sst::overlay::OverlayTimeline>;

}  // namespace sst::adapters::overlay
