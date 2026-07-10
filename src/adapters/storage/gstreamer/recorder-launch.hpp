#pragma once

#include <string>

#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::storage {

// Default encode framerate + bitrate when the app supplies no explicit record
// quality (VideoQuality::IsSet() == false). 30 fps / 8 Mbit/s is the software
// x264 baseline sized to the dual-camera CPU budget on the Orin Nano.
inline constexpr int kRecorderDefaultFramerate = 30;
inline constexpr int kRecorderBitrateKbps = 14000;

// Quality profile: drop tune=zerolatency and spend a small reorder budget on
// B-frames + rate-control lookahead — the biggest quality-per-bit levers with no
// NVENC. The reorder buffer is only a few frames (not a deep hold), so Stop's
// moov finalize drains it well within kFinalizeTimeoutSeconds. Record isn't
// latency-sensitive (it's a file), so this is pure quality gain.
inline constexpr int kRecorderBframes = 3;
inline constexpr int kRecorderRcLookahead = 20;

// Bound on the leaky pre-encoder queue (~0.5s at 60fps). Drop-oldest under
// encode overload keeps the encoder realtime so the MP4 always finalizes a valid
// moov, at the cost of dropped frames rather than a corrupt file.
inline constexpr int kRecorderQueueMaxBuffers = 30;

// Names the appsrc and encoder elements so GstContinuousRecorder can look them
// up after gst_parse_launch.
inline constexpr const char* kRecorderAppsrcName = "src";
inline constexpr const char* kRecorderEncoderName = "enc";

// Builds the GStreamer launch string for the continuous MP4 recorder, targeting
// `output_mp4` and applying the app-supplied record `quality`. Pure (no GES /
// element instantiation) so the pipeline shape is unit-testable without hardware.
//
// When `quality.IsSet()`, a per-branch `videoscale ! videorate` scales the
// incoming (postprocess-output) frame to the requested resolution/fps before
// x264enc — resolution/fps independence is realized here, NOT by changing the
// shared capture caps, so the raw dual-recording branch is unaffected. When
// quality is unset the recorder keeps the source resolution at the default fps.
//
// The `video/x-raw,format=I420` cap forces 4:2:0 chroma before x264enc — without
// it, BGR/RGB input encodes High 4:4:4 which Android and most hardware decoders
// cannot play.
//
// `use_vic` selects the scale + colour-convert element: false (default) uses the
// proven software `videoconvert ! videoscale` path; true offloads it to the
// Jetson VIC (`nvvidconv`, hardware) to free CPU for the software encoders (U2).
// The default stays software off-metal — the VIC path is enabled + validated by
// the on-metal combined-load spike (record + RTMP + preview + 2 proxies), which
// also gates advertising 1080p60 (U3). `videorate` stays software either way
// (VIC does not resample framerate).
[[nodiscard]] auto BuildRecorderLaunch(const std::string& output_mp4,
                                       const sst::common::VideoQuality& quality,
                                       bool use_vic = false) -> std::string;

}  // namespace sst::adapters::storage
