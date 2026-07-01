#pragma once

#include <gst/gst.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#include "app/storage/ports/continuous-recorder.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::storage {

// Single continuous-MP4 recorder. Software H.264 (the Orin Nano has no NVENC);
// still hardware-bound for tests (needs a live GStreamer pipeline + x264enc).
//
//   appsrc → videoconvert → [videoscale → videorate] → x264enc → h264parse
//         → mp4mux → filesink
//
// The optional videoscale/videorate stage applies an app-supplied record
// resolution/fps (Start's VideoQuality) independently of the stream and raw
// branches.
//
// Pause stops feeding the muxer; Resume forces a key-unit and feeds the SAME
// file again (drop-to-IDR). Stop sends EOS and waits for the muxer to write a
// playable moov atom.
class GstContinuousRecorder final : public sst::storage::IContinuousRecorder {
   public:
    GstContinuousRecorder();
    ~GstContinuousRecorder() override;

    GstContinuousRecorder(const GstContinuousRecorder&) = delete;
    auto operator=(const GstContinuousRecorder&) -> GstContinuousRecorder& = delete;
    GstContinuousRecorder(GstContinuousRecorder&&) = delete;
    auto operator=(GstContinuousRecorder&&) -> GstContinuousRecorder& = delete;

    auto Start(const std::filesystem::path& output_mp4,
               const sst::common::VideoQuality& quality) -> bool override;
    auto Pause() -> void override;
    auto Resume() -> void override;
    auto Stop() -> bool override;
    [[nodiscard]] auto IsRunning() const -> bool override;
    auto Push(const sst::capture::Frame& frame) -> void override;

   private:
    auto Teardown() -> void;

    mutable std::mutex mtx_;
    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    GstElement* encoder_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool caps_set_{false};
    // Record resolution/fps for this session; drives the appsrc input framerate
    // cap (the videoscale/videorate target lives in the launch string).
    sst::common::VideoQuality quality_;
};

}  // namespace sst::adapters::storage
