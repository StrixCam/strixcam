#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/overlay/ports/overlay-burner.hpp"
#include "domain/network/models/download-token.hpp"
#include "domain/overlay/models/overlay-timeline.hpp"

namespace sst::exportjob {

// Lifecycle of one overlay-burn job (mirrors proto ExportJobState).
enum class ExportState : std::uint8_t { kPending, kRunning, kReady, kFailed };

// A snapshot of a job for polling.
struct ExportJobView {
    ExportState state{ExportState::kPending};
    std::optional<sst::network::DownloadToken> token;  // set iff state == kReady
    std::string error;                                 // set iff state == kFailed
};

// Runs on-demand overlay burns (#6 F6c) as background jobs. Each RequestExport
// spawns a worker that resolves the clean L1, loads its overlay timeline, burns
// the overlaid L2 PERSISTENTLY into the SAME per-match directory as the L1
// (<l1_stem>-overlay.mp4 — see overlay-export-naming.hpp; no separate exports
// dir, no post-download deletion), and mints a download token. The L2 then
// enumerates and downloads like any recording; re-exporting an already-burned
// recording returns the existing file without re-encoding. The collaborators
// are injected as functions so the manager stays in the app layer (no
// adapter/network deps) and is fully unit-testable with fakes.
//
// The live-session gate (#6: never burn while recording/streaming) is checked by
// the caller (ExportBurnHandler) BEFORE RequestExport and RE-checked on the
// worker just before encoding (via the optional `is_live` predicate) so a match
// that starts in the gap still aborts the burn.
class ExportJobManager {
   public:
    // recording_id -> the clean L1 file, or nullopt if unknown.
    using PathResolver =
        std::function<std::optional<std::filesystem::path>(const std::string& recording_id)>;
    // L1 file -> its overlay timeline (empty timeline => the burn passes frames
    // through unchanged, i.e. a clean L2).
    using TimelineLoader =
        std::function<sst::overlay::OverlayTimeline(const std::filesystem::path& l1_path)>;
    // (L2 file, its file id = the L2 stem) -> a download token, or nullopt on
    // failure. The id is the stem so the token downloads the L2 exactly like a
    // listed recording.
    using TokenMinter = std::function<std::optional<sst::network::DownloadToken>(
        const std::filesystem::path& l2_path, const std::string& file_id)>;
    // True while a match is live. Re-checked just before the burn starts so a
    // recording that begins after the request still aborts the encode.
    using LiveGate = std::function<bool()>;

    ExportJobManager(sst::overlay::IOverlayBurner& burner, PathResolver resolve,
                     TimelineLoader load_timeline, TokenMinter mint);
    ~ExportJobManager();

    ExportJobManager(const ExportJobManager&) = delete;
    auto operator=(const ExportJobManager&) -> ExportJobManager& = delete;
    ExportJobManager(ExportJobManager&&) = delete;
    auto operator=(ExportJobManager&&) -> ExportJobManager& = delete;

    // Queue a burn for `recording_id`; returns a job id immediately (the burn
    // runs on a worker thread). If an unfinished/ready burn for the same
    // recording already exists it is reused instead of spawning a duplicate;
    // if the persistent L2 already exists on disk the worker skips the encode
    // and goes straight to ready. `is_live`, when set, is re-checked on the
    // worker just before encoding so a match that goes live after the request
    // aborts the burn.
    auto RequestExport(const std::string& recording_id, LiveGate is_live = nullptr) -> std::string;

    // Current state of `job_id`, or nullopt if there is no such job.
    [[nodiscard]] auto Poll(const std::string& job_id) const -> std::optional<ExportJobView>;

   private:
    struct Job {
        std::string id;
        std::string recording_id;
        std::atomic<ExportState> state{ExportState::kPending};
        std::atomic<bool> cancel{false};  // set on shutdown to abort an in-flight burn
        LiveGate is_live;                 // re-checked before encoding (may be null)
        mutable std::mutex mtx;           // guards token/error
        std::optional<sst::network::DownloadToken> token;
        std::string error;
        std::thread worker;
    };

    void Run(Job& job);

    // Cap retained jobs: drop the oldest finished job RECORDS so a long-lived
    // camera doesn't accumulate jobs/threads without bound. The burned L2 files
    // are persistent recordings and are never deleted here.
    // Caller must hold jobs_mtx_.
    void EvictFinishedLocked();

    static constexpr std::size_t kMaxJobs = 16;

    sst::overlay::IOverlayBurner& burner_;
    PathResolver resolve_;
    TimelineLoader load_timeline_;
    TokenMinter mint_;

    mutable std::mutex jobs_mtx_;
    std::atomic<std::uint64_t> next_id_{0};
    std::vector<std::shared_ptr<Job>> jobs_;
};

}  // namespace sst::exportjob
