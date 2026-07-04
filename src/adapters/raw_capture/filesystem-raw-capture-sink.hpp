#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/raw_capture/ports/raw-capture-sink.hpp"
#include "domain/capture/models/frame.hpp"

// Forward-declared to keep the GStreamer headers out of this (widely-included)
// header; the .cpp includes <gst/gst.h>.
using GstElement = struct _GstElement;

namespace sst::adapters::raw_capture {

// Per-camera training-proxy sink. One small H.264 MP4 per camera under
// `video_dir`, named raw__<group>__cam<N>.mp4 (see raw-capture-naming.hpp), each
// a GStreamer pipeline (appsrc -> scale to kProxyVideoMode -> x264enc -> mp4mux
// -> filesink, see proxy-launch.hpp). Replaces the earlier uncompressed-NV12
// writer: raw NV12 was ~93 MB/s per camera (~186 MB/s for two) and unusable for
// "small training footage", so the proxy compresses to a low-bitrate H.264.
//
// This sink is fed from the shared capture/fan-out thread (PipelineOrchestrator),
// so PushCamera MUST be non-blocking: it hands the frame to the per-camera
// appsrc (block=false) behind a leaky=downstream queue that drops oldest under
// encode overload, and uses try_to_lock so a concurrent Start/Stop can never
// stall capture. Stop detaches the pipelines under the lock (O(1)) and does the
// EOS + moov-finalize wait OUTSIDE the lock so it never freezes the fan-out.
class FilesystemRawCaptureSink final : public sst::raw_capture::IRawCaptureSink {
   public:
    FilesystemRawCaptureSink(std::filesystem::path video_dir, std::uint32_t camera_count);
    ~FilesystemRawCaptureSink() override;

    FilesystemRawCaptureSink(const FilesystemRawCaptureSink&) = delete;
    auto operator=(const FilesystemRawCaptureSink&) -> FilesystemRawCaptureSink& = delete;
    FilesystemRawCaptureSink(FilesystemRawCaptureSink&&) = delete;
    auto operator=(FilesystemRawCaptureSink&&) -> FilesystemRawCaptureSink& = delete;

    auto Start(const std::string& capture_group_id,
               const std::filesystem::path& output_dir) -> bool override;
    auto PushCamera(std::uint32_t camera_index, const sst::capture::Frame& frame) -> void override;
    auto Stop() -> bool override;
    [[nodiscard]] auto IsCapturing() const -> bool override;

   private:
    // Per-camera H.264 encode pipeline. appsrc is looked up by name after
    // gst_parse_launch; caps_set defers the appsrc input caps to the first frame
    // (its real NV12 geometry).
    struct CameraWriter {
        GstElement* pipeline{nullptr};
        GstElement* appsrc{nullptr};
        bool caps_set{false};
    };

    // Send EOS, wait (bounded) for mp4mux to finalize the moov, set NULL, unref.
    // Runs OUTSIDE mtx_ (from Stop / the dtor).
    static auto TeardownWriter(CameraWriter& writer) -> void;

    const std::filesystem::path video_dir_;
    const std::uint32_t camera_count_;
    const bool use_vic_;

    mutable std::mutex mtx_;
    std::atomic<bool> capturing_{false};
    std::string capture_group_id_;
    std::vector<std::unique_ptr<CameraWriter>> writers_;
};

}  // namespace sst::adapters::raw_capture
