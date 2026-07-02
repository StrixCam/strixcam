#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/storage/ports/continuous-recorder.hpp"
#include "app/storage/ports/disk-guard.hpp"
#include "app/storage/ports/recording-service.hpp"
#include "app/storage/ports/thumbnail-writer.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/storage/models/recording-state.hpp"

namespace sst::storage {

// Owns the recording state machine over a single continuous MP4 (KTD6). Pushes
// postprocessed frames into the continuous recorder; PAUSE/RESUME gate the
// muxer into the same file; STOP and disconnect-finalize both EOS the muxer to
// a playable file and write a disk thumbnail from the last seen frame.
//
// Threading: Push() runs on the producer (orchestration) thread; the lifecycle
// methods run on the BLE thread. Both share `mtx_`.
class RecordingService final : public IRecordingService, public sst::buffer::IFrameSink {
   public:
    RecordingService(std::unique_ptr<IContinuousRecorder> recorder,
                     std::unique_ptr<IThumbnailWriter> thumbnail_writer, IDiskGuard& disk_guard);

    ~RecordingService() override;

    RecordingService(const RecordingService&) = delete;
    auto operator=(const RecordingService&) -> RecordingService& = delete;
    RecordingService(RecordingService&&) = delete;
    auto operator=(RecordingService&&) -> RecordingService& = delete;

    // IRecordingService
    auto StartRecording(const std::string& video_output_path,
                        const std::string& thumbnail_output_path,
                        const sst::common::VideoQuality& quality) -> bool override;
    auto Pause() -> bool override;
    auto Resume() -> bool override;
    auto Stop() -> StopRecordingResult override;
    [[nodiscard]] auto CurrentState() const -> RecordingState override;

    // IFrameSink
    auto Push(const sst::capture::Frame& frame) -> void override;

   private:
    std::unique_ptr<IContinuousRecorder> recorder_;
    std::unique_ptr<IThumbnailWriter> thumbnail_writer_;
    IDiskGuard& disk_guard_;

    mutable std::mutex mtx_;
    RecordingState state_{RecordingState::kIdle};
    std::filesystem::path video_path_;
    std::filesystem::path thumbnail_path_;
    std::optional<sst::capture::Frame> last_frame_;
};

}  // namespace sst::storage
