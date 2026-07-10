#pragma once

#include <string>

#include "domain/streaming/models/platform-stream-config.hpp"

namespace sst::adapters::streaming {

inline constexpr const char* kRtmpAppsrcName = "src";

// Bound on the leaky pre-encoder queue (~0.5s at 60fps). Drop-oldest keeps the
// software encoder from accumulating raw frames unbounded in the is-live appsrc
// when it falls behind realtime (no NVENC).
inline constexpr int kRtmpPreEncodeQueueBuffers = 30;

// Quality-profile tuning for the stream encode (mirrors the recorder). The
// stream drops tune=zerolatency and spends the operator's accepted 30–60 s
// output delay on B-frames + rate-control lookahead — the biggest
// quality-per-bit levers with no NVENC. Starting values; the slowest sustainable
// point is measured on metal (record + stream + preview concurrently) and these
// are the committed defaults, dialable at runtime via SST_STREAM_BITRATE_KBPS /
// SST_STREAM_QUEUE_MS.
inline constexpr int kRtmpBframes = 3;
inline constexpr int kRtmpRcLookahead = 20;

// Time-based deepening (ms) of the leaky pre-encode queue so a brief sub-realtime
// dip is absorbed as buffered frames rather than dropped; leaky=downstream stays
// the sustained-overload backstop. Kept well below a literal 30 s raw hold (RAM +
// match-end flush time bound it). Dialable on metal via SST_STREAM_QUEUE_MS.
inline constexpr int kRtmpPreEncodeQueueMaxTimeMs = 3000;

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
//
// `use_vic` selects the scale + colour-convert element: false (default) is the
// proven software `videoconvert ! videoscale`; true offloads to the Jetson VIC
// (`nvvidconv`) to free CPU for the shared software encoders (U2). Default stays
// software off-metal — enabled + validated by the on-metal combined-load spike.
[[nodiscard]] auto BuildRtmpLaunch(const sst::streaming::PlatformStreamConfig& cfg,
                                   bool use_vic = false) -> std::string;

}  // namespace sst::adapters::streaming
