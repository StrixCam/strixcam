#pragma once

#include <filesystem>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/video-quality.hpp"

namespace sst::storage {

// One continuous, playable MP4 per session. Pause/Resume gate the muxer into the
// SAME file (drop-to-IDR on resume); Stop EOSes the muxer to a finalized,
// seekable file. Implemented by a GStreamer NVENC adapter (hardware-bound).
class IContinuousRecorder {
   public:
    virtual ~IContinuousRecorder() = default;

    // `quality` sets the record resolution/fps (VideoQuality::IsSet() == false
    // keeps the source resolution at the default framerate). Applied at pipeline
    // (re)build via a per-branch scaler — the raw dual-recording is unaffected.
    virtual auto Start(const std::filesystem::path& output_mp4,
                       const sst::common::VideoQuality& quality) -> bool = 0;
    virtual auto Pause() -> void = 0;
    virtual auto Resume() -> void = 0;
    // EOS the muxer and finalize a playable file. Returns false if not running.
    virtual auto Stop() -> bool = 0;
    [[nodiscard]] virtual auto IsRunning() const -> bool = 0;

    virtual auto Push(const sst::capture::Frame& frame) -> void = 0;
};

}  // namespace sst::storage
