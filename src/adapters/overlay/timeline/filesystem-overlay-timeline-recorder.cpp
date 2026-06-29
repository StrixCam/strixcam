#include "adapters/overlay/timeline/filesystem-overlay-timeline-recorder.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "adapters/overlay/timeline/serde/render-scene-serde.hpp"

namespace sst::adapters::overlay {

namespace {

namespace fs = std::filesystem;

constexpr const char* kTimelineExt = ".timeline.json";

// The match id is the recording directory's name (the app hands a per-match
// dir, possibly with a trailing slash). Mirrors recording-service's stem rule so
// the timeline sits beside `<matchId>.mp4` as `<matchId>.timeline.json`.
auto MatchIdFromDir(const fs::path& dir) -> std::string {
    return dir.has_filename() ? dir.filename().string() : dir.parent_path().filename().string();
}

// Compose the timeline path from the same `video_output_path` the recorder gets,
// mirroring RecordingService::ComposeFile so the sidecar always lands beside the
// L1 and matches the export loader's `<parent>/<stem>.timeline.json` derivation
// — whether the app hands a per-match directory or a concrete file path.
auto ComposeTimelinePath(const fs::path& dir_or_file) -> fs::path {
    const std::string str = dir_or_file.string();
    if (!str.empty() && str.back() != '/' && dir_or_file.has_extension()) {
        // Concrete file (e.g. <matchId>.mp4): sibling <stem>.timeline.json.
        return dir_or_file.parent_path() / (dir_or_file.stem().string() + kTimelineExt);
    }
    const std::string match_id = MatchIdFromDir(dir_or_file);
    return dir_or_file / ((match_id.empty() ? "recording" : match_id) + kTimelineExt);
}

}  // namespace

auto FilesystemOverlayTimelineRecorder::Start(const fs::path& match_dir,
                                              std::uint64_t anchor_ms) -> void {
    const std::lock_guard lock(mtx_);
    timeline_path_ = ComposeTimelinePath(match_dir);
    anchor_ms_ = anchor_ms;
    events_.clear();
    active_ = true;
    spdlog::info("OverlayTimeline: capturing -> {} (anchor_ms={})", timeline_path_.string(),
                 anchor_ms_);
}

auto FilesystemOverlayTimelineRecorder::OnScene(std::uint64_t at_ms,
                                                const sst::overlay::RenderScene& scene) -> void {
    const std::lock_guard lock(mtx_);
    if (!active_) {
        return;  // not recording — the live stream still shows the overlay, we
                 // just don't persist a timeline outside a recording.
    }
    events_.push_back(Entry{at_ms, scene});
}

auto FilesystemOverlayTimelineRecorder::Stop() -> void {
    const std::lock_guard lock(mtx_);
    if (!active_) {
        return;
    }
    active_ = false;
    if (events_.empty()) {
        spdlog::info("OverlayTimeline: no overlay scenes captured; not writing {}",
                     timeline_path_.string());
        return;
    }

    nlohmann::json doc;
    doc["anchor_ms"] = anchor_ms_;
    nlohmann::json events = nlohmann::json::array();
    for (const auto& entry : events_) {
        events.push_back(nlohmann::json{{"at_ms", entry.at_ms}, {"scene", entry.scene}});
    }
    doc["events"] = std::move(events);

    std::ofstream out(timeline_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("OverlayTimeline: cannot open {} for writing", timeline_path_.string());
        events_.clear();
        return;
    }
    out << doc.dump();
    if (!out) {
        spdlog::error("OverlayTimeline: write failed for {}", timeline_path_.string());
    } else {
        spdlog::info("OverlayTimeline: wrote {} scene(s) -> {}", events_.size(),
                     timeline_path_.string());
    }
    events_.clear();
}

}  // namespace sst::adapters::overlay
