#pragma once

#include <string>

#include "domain/streaming/models/platform-stream-config.hpp"

namespace sst::adapters::streaming {

inline constexpr const char* kRtmpAppsrcName = "src";

// Builds the "<url>/<key>" location rtmp2sink expects — most ingest endpoints
// accept the stream key as the final path segment. rtmp:// and rtmps:// use the
// same element; only the URL scheme differs.
[[nodiscard]] auto BuildRtmpLocation(const sst::streaming::PlatformStreamConfig& cfg)
    -> std::string;

// Builds the RTMP push pipeline launch string. Pure (no element instantiation)
// so the pipeline shape is unit-testable without hardware.
//
// The appsrc carries NO fixed input caps — GstRtmpStreamer sets them lazily from
// the first frame's real geometry/format. A per-branch `videoscale ! videorate`
// then conforms that source frame to the app-requested stream resolution/fps
// (cfg.width/height/framerate), independent of the record and raw branches. A
// silent AAC track rides alongside because platforms (YouTube) reject video-only
// FLV.
[[nodiscard]] auto BuildRtmpLaunch(const sst::streaming::PlatformStreamConfig& cfg) -> std::string;

}  // namespace sst::adapters::streaming
