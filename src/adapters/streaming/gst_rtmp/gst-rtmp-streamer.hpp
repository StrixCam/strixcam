#pragma once

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "app/streaming/ports/platform-streamer.hpp"

namespace sst::adapters::streaming {

// Per-platform RTMP/RTMPS push pipeline (software encode — the Orin Nano has no
// NVENC):
//
//   appsrc → videoconvert → x264enc → h264parse → queue(leaky) ─┐
//   audiotestsrc(silence) → voaacenc → aacparse ────────────────┤
//                                  flvmux(name=mux) → rtmp2sink location=<url>/<key>
//
// rtmp2sink + voaacenc ship in `gstreamer1.0-plugins-bad`; x264enc in
// `plugins-ugly`. A silent AAC track is required because platforms (YouTube)
// reject video-only FLV. rtmp2sink has no auto-reconnect, so a watcher thread
// rebuilds the pipeline on a bus ERROR (uplink drop) without disturbing the rest
// of the capture/preview path. The same element handles rtmp:// and rtmps:// —
// the scheme rides in the `location` URL.
class GstRtmpStreamer final : public sst::streaming::IPlatformStreamer {
   public:
    GstRtmpStreamer();
    ~GstRtmpStreamer() override;

    GstRtmpStreamer(const GstRtmpStreamer&) = delete;
    auto operator=(const GstRtmpStreamer&) -> GstRtmpStreamer& = delete;
    GstRtmpStreamer(GstRtmpStreamer&&) = delete;
    auto operator=(GstRtmpStreamer&&) -> GstRtmpStreamer& = delete;

    auto Start(const sst::streaming::PlatformStreamConfig& config) -> bool override;
    auto Stop() -> void override;
    [[nodiscard]] auto IsRunning() const -> bool override;
    [[nodiscard]] auto Id() const -> std::int64_t override;

    auto Push(const sst::capture::Frame& frame) -> void override;

   private:
    auto Teardown() -> void;
    // Build the pipeline from config_ and transition it to PLAYING. Caller holds
    // mtx_. Returns false (and tears down) on any failure.
    auto BuildAndPlayLocked() -> bool;
    // Polls the active pipeline's bus for ERROR and rebuilds on uplink drop.
    auto WatcherLoop() -> void;

    sst::streaming::PlatformStreamConfig config_{};

    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::atomic<bool> watching_{false};
    std::thread watcher_;

    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    // Whether the appsrc input caps have been set from a frame this pipeline
    // life. Reset on every (re)build so a reconnect re-derives them from the next
    // frame's real geometry/format. Guarded by mtx_.
    bool caps_set_{false};
};

}  // namespace sst::adapters::streaming
