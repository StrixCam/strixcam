#include "app/storage/services/recording_service/recording-service.hpp"

#include <spdlog/spdlog.h>

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
// fail), leaving an empty match dir. Compose the file inside the directory.
// Tolerate a path that already names a file (has an extension / no trailing /).
auto ComposeFile(const std::string& dir_or_file, const char* filename) -> std::filesystem::path {
    if (dir_or_file.empty()) {
        return {};
    }
    const std::filesystem::path p(dir_or_file);
    const bool looks_like_dir = dir_or_file.back() == '/' || !p.has_extension();
    return looks_like_dir ? p / filename : p;
}

// Clean layer-1 recording (raw, no overlay). Overlaid L2 is burned on demand.
constexpr const char* kCleanRecordingName = "l1.mp4";
constexpr const char* kThumbnailName = "l1.jpg";

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
                                      const std::string& thumbnail_output_path) -> bool {
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
    video_path_ = ComposeFile(video_output_path, kCleanRecordingName);
    thumbnail_path_ = ComposeFile(thumbnail_output_path, kThumbnailName);

    std::error_code ec;
    if (!video_path_.parent_path().empty()) {
        std::filesystem::create_directories(video_path_.parent_path(), ec);
    }

    if (!recorder_->Start(video_path_)) {
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
