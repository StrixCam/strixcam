#include "app/storage/services/recording_service/recording-service.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include "domain/storage/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::storage {

namespace {

// The session contract hands us a per-match DIRECTORY (e.g.
// /var/lib/sst/cam/videos/<user>/<match>/), not a file. The recorder's filesink
// and the thumbnail's cv::imwrite both need a concrete file path — handing them
// the directory makes the GStreamer pipeline fail to reach PLAYING (and imwrite
// fail), leaving an empty match dir.
//
// The file must be named <matchId>.<ext>: the DownloadServer keys recordings by
// file *stem* and the app requests downloads by match id, so the stem has to be
// the match id. The match id is the directory's own name. Tolerate a path that
// already names a file (has an extension / no trailing slash) by using it as-is.
auto MatchIdFromDir(const std::filesystem::path& dir) -> std::string {
    // A trailing slash makes filename() empty, so fall back to the parent.
    return dir.has_filename() ? dir.filename().string() : dir.parent_path().filename().string();
}

auto ComposeFile(const std::string& dir_or_file, const char* ext) -> std::filesystem::path {
    if (dir_or_file.empty()) {
        return {};
    }
    std::filesystem::path path(dir_or_file);
    if (dir_or_file.back() != '/' && path.has_extension()) {
        return path;  // already a concrete file
    }
    const std::string match_id = MatchIdFromDir(path);
    return path / ((match_id.empty() ? "recording" : match_id) + ext);
}

constexpr const char* kVideoExt = ".mp4";
constexpr const char* kThumbnailExt = ".jpg";

}  // namespace

RecordingService::RecordingService(std::unique_ptr<IContinuousRecorder> recorder,
                                   std::unique_ptr<IThumbnailWriter> thumbnail_writer,
                                   IDiskGuard& disk_guard)
    : recorder_(std::move(recorder)),
      thumbnail_writer_(std::move(thumbnail_writer)),
      disk_guard_(disk_guard) {}

RecordingService::~RecordingService() {
    if (state_ != RecordingState::kIdle) {
        spdlog::warn("RecordingService: destructor while state={} — finalizing", state_);
        (void)Stop();
    }
}

auto RecordingService::StartRecording(const std::string& video_output_path,
                                      const std::string& thumbnail_output_path,
                                      const sst::common::VideoQuality& quality) -> bool {
    std::lock_guard lock(mtx_);
    if (state_ != RecordingState::kIdle) {
        spdlog::warn("RecordingService::StartRecording rejected: state={}", state_);
        return false;
    }
    if (video_output_path.empty()) {
        spdlog::error("RecordingService::StartRecording: empty video output path");
        return false;
    }
    if (!disk_guard_.HasEnoughFreeSpace()) {
        spdlog::warn("RecordingService::StartRecording rejected: disk guard refused (free={})",
                     disk_guard_.FreeBytes());
        return false;
    }

    // Compose concrete file paths inside the per-match directories the session
    // hands us; filesink / imwrite cannot write to a bare directory.
    video_path_ = ComposeFile(video_output_path, kVideoExt);
    thumbnail_path_ = ComposeFile(thumbnail_output_path, kThumbnailExt);

    // Refuse to clobber an existing recording for this match. A recorder Start
    // points filesink at <matchId>.mp4; a STOP→START within the same match
    // re-composes the SAME path, so without this guard the second take
    // overwrites (and destroys) the first — the "only the last segment is
    // saved" data-loss. Mid-match continuation must go through PAUSE/RESUME
    // (one continuous file), not STOP→START. The threshold tolerates a tiny
    // stub left by a failed prior Start without blocking a legitimate retry.
    {
        std::error_code exists_ec;
        constexpr std::uintmax_t kMinExistingBytes = std::uintmax_t{64} * 1024;
        const auto existing = std::filesystem::file_size(video_path_, exists_ec);
        if (!exists_ec && existing >= kMinExistingBytes) {
            spdlog::warn(
                "RecordingService::StartRecording refused: {} already holds {} bytes — "
                "use PAUSE/RESUME to continue a match, not STOP/START",
                video_path_.string(), existing);
            video_path_.clear();
            thumbnail_path_.clear();
            return false;
        }
    }

    std::error_code mkdir_ec;
    if (!video_path_.parent_path().empty()) {
        std::filesystem::create_directories(video_path_.parent_path(), mkdir_ec);
    }

    if (!recorder_->Start(video_path_, quality)) {
        spdlog::error("RecordingService::StartRecording: recorder failed to start ({})",
                      video_path_.string());
        video_path_.clear();
        thumbnail_path_.clear();
        return false;
    }

    state_ = RecordingState::kRecording;
    spdlog::info("RecordingService: recording -> {}", video_path_.string());
    return true;
}

auto RecordingService::Pause() -> bool {
    std::lock_guard lock(mtx_);
    if (state_ != RecordingState::kRecording) {
        spdlog::warn("RecordingService::Pause rejected: state={}", state_);
        return false;
    }
    recorder_->Pause();
    state_ = RecordingState::kPaused;
    spdlog::info("RecordingService: paused (same file resumes on RESUME)");
    return true;
}

auto RecordingService::Resume() -> bool {
    std::lock_guard lock(mtx_);
    if (state_ != RecordingState::kPaused) {
        spdlog::warn("RecordingService::Resume rejected: state={}", state_);
        return false;
    }
    recorder_->Resume();
    state_ = RecordingState::kRecording;
    spdlog::info("RecordingService: resumed");
    return true;
}

auto RecordingService::Stop() -> StopRecordingResult {
    std::lock_guard lock(mtx_);
    if (state_ == RecordingState::kIdle) {
        return {.success = false, .file_path = {}, .thumbnail_written = false};
    }

    const bool stopped = recorder_->Stop();

    bool thumb = false;
    if (last_frame_ && !thumbnail_path_.empty()) {
        thumb = thumbnail_writer_->Write(*last_frame_, thumbnail_path_);
        if (!thumb) {
            spdlog::warn("RecordingService: thumbnail write failed ({})", thumbnail_path_.string());
        }
    }

    const std::filesystem::path file = video_path_;
    spdlog::info("RecordingService: finalized {} (stopped={}, thumbnail={})", file.string(),
                 stopped, thumb);

    state_ = RecordingState::kIdle;
    video_path_.clear();
    thumbnail_path_.clear();
    last_frame_.reset();

    return {.success = stopped, .file_path = file, .thumbnail_written = thumb};
}

auto RecordingService::CurrentState() const -> RecordingState {
    std::lock_guard lock(mtx_);
    return state_;
}

auto RecordingService::Push(const sst::capture::Frame& frame) -> void {
    std::lock_guard lock(mtx_);
    if (state_ == RecordingState::kIdle) {
        return;
    }
    // Keep the most recent frame for the finalization thumbnail.
    last_frame_ = frame;
    // Feed the muxer; the recorder drops frames internally while paused and
    // resumes at the next IDR into the same file.
    recorder_->Push(frame);
}

}  // namespace sst::storage
