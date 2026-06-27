#pragma once

#include <atomic>
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
// the overlaid L2, and mints a download token. The collaborators are injected as
// functions so the manager stays in the app layer (no adapter/network deps) and
// is fully unit-testable with fakes.
//
// The live-session gate (#6: never burn while recording/streaming) is enforced
// by the caller (ExportBurnHandler) BEFORE RequestExport — the manager assumes
// it is safe to encode.
class ExportJobManager {
   public:
    // recording_id -> the clean L1 file, or nullopt if unknown.
    using PathResolver =
        std::function<std::optional<std::filesystem::path>(const std::string& recording_id)>;
    // L1 file -> its overlay timeline (empty timeline => the burn passes frames
    // through unchanged, i.e. a clean L2).
    using TimelineLoader =
        std::function<sst::overlay::OverlayTimeline(const std::filesystem::path& l1_path)>;
    // (L2 file, job id) -> a download token, or nullopt on failure.
    using TokenMinter = std::function<std::optional<sst::network::DownloadToken>(
        const std::filesystem::path& l2_path, const std::string& job_id)>;

    ExportJobManager(sst::overlay::IOverlayBurner& burner, PathResolver resolve,
                     TimelineLoader load_timeline, TokenMinter mint,
                     std::filesystem::path export_dir);
    ~ExportJobManager();

    ExportJobManager(const ExportJobManager&) = delete;
    auto operator=(const ExportJobManager&) -> ExportJobManager& = delete;
    ExportJobManager(ExportJobManager&&) = delete;
    auto operator=(ExportJobManager&&) -> ExportJobManager& = delete;

    // Queue a burn for `recording_id`; returns the new job id immediately (the
    // burn runs on a worker thread).
    auto RequestExport(const std::string& recording_id) -> std::string;

    // Current state of `job_id`, or nullopt if there is no such job.
    [[nodiscard]] auto Poll(const std::string& job_id) const -> std::optional<ExportJobView>;

   private:
    struct Job {
        std::string id;
        std::string recording_id;
        std::atomic<ExportState> state{ExportState::kPending};
        mutable std::mutex mtx;  // guards token/error
        std::optional<sst::network::DownloadToken> token;
        std::string error;
        std::thread worker;
    };

    void Run(Job& job);

    sst::overlay::IOverlayBurner& burner_;
    PathResolver resolve_;
    TimelineLoader load_timeline_;
    TokenMinter mint_;
    std::filesystem::path export_dir_;

    mutable std::mutex jobs_mtx_;
    std::atomic<std::uint64_t> next_id_{0};
    std::vector<std::shared_ptr<Job>> jobs_;
};

}  // namespace sst::exportjob
