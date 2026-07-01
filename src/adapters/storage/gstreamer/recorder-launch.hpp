#pragma once

#include <string>

#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::storage {

// Default encode framerate + bitrate when the app supplies no explicit record
// quality (VideoQuality::IsSet() == false). 30 fps / 8 Mbit/s is the software
// x264 baseline sized to the dual-camera CPU budget on the Orin Nano.
inline constexpr int kRecorderDefaultFramerate = 30;
inline constexpr int kRecorderBitrateKbps = 8000;

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
[[nodiscard]] auto BuildRecorderLaunch(const std::string& output_mp4,
                                       const sst::common::VideoQuality& quality) -> std::string;

}  // namespace sst::adapters::storage
