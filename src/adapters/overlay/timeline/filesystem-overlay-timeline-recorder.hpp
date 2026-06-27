#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

#include "app/overlay/ports/overlay-timeline-recorder.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace sst::adapters::overlay {

// Filesystem implementation of IOverlayTimelineRecorder. Accumulates pushed
// scenes in memory during a recording and writes them to
// `<match_dir>/<matchId>.timeline.json` on Stop (alongside the L1 MP4, never
// auto-deleted — kept for re-export). Thread-safe: OnScene runs on the overlay
// threads while Start/Stop run on the BLE command thread.
//
// File schema:
//   { "anchor_ms": <uint64>,
//     "events": [ { "at_ms": <uint64>, "scene": <RenderScene> }, ... ] }
class FilesystemOverlayTimelineRecorder final : public sst::overlay::IOverlayTimelineRecorder {
   public:
    auto Start(const std::filesystem::path& match_dir, std::uint64_t anchor_ms) -> void override;
    auto OnScene(std::uint64_t at_ms, const sst::overlay::RenderScene& scene) -> void override;
    auto Stop() -> void override;

   private:
    struct Entry {
        std::uint64_t at_ms{0};
        sst::overlay::RenderScene scene;
    };

    mutable std::mutex mtx_;
    bool active_{false};
    std::filesystem::path timeline_path_;
    std::uint64_t anchor_ms_{0};
    std::vector<Entry> events_;
};

}  // namespace sst::adapters::overlay
