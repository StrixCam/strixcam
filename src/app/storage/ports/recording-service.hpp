#pragma once

#include <filesystem>
#include <string>

#include "domain/common/models/video-quality.hpp"
#include "domain/storage/models/recording-state.hpp"

namespace sst::storage {

struct StopRecordingResult {
    bool success{false};
    std::filesystem::path file_path;
    bool thumbnail_written{false};
};

// Drives the single continuous-MP4 recorder for a session. START/STOP/PAUSE/
// RESUME map to the contract RecordingControl actions; Stop() (and the
// disconnect-finalize path) EOSes the muxer to a playable file and writes a
// disk thumbnail. Implements IFrameSink internally so postprocess output is
// pushed straight in.
class IRecordingService {
   public:
    virtual ~IRecordingService() = default;

    // Begin a continuous recording to the app-supplied output paths at the
    // app-supplied record `quality` (unset => firmware default resolution/fps).
    virtual auto StartRecording(const std::string& video_output_path,
                                const std::string& thumbnail_output_path,
                                const sst::common::VideoQuality& quality) -> bool = 0;

    virtual auto Pause() -> bool = 0;
    virtual auto Resume() -> bool = 0;

    // Finalize the recording (EOS + thumbnail). Idempotent — a no-op when idle,
    // so the disconnect cleanup path can call it unconditionally.
    virtual auto Stop() -> StopRecordingResult = 0;

    [[nodiscard]] virtual auto CurrentState() const -> RecordingState = 0;
};

}  // namespace sst::storage
