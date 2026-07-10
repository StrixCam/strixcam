#pragma once

#include <cstdint>
#include <string>

namespace sst::streaming {

enum class PlatformStreamType : std::uint8_t { kRtmp = 0, kRtmpS = 1 };
enum class PlatformStreamCodec : std::uint8_t { kH264 = 0, kH265 = 1 };

// Default 1080p30 @ 14 Mbit/s — the platform-stream baseline when the app
// supplies no explicit encode parameters. 30 (not 60) fps because the Orin Nano
// has no NVENC and software x264enc cannot sustain 1080p60 in realtime (see
// video-quality.hpp) — 1080p60 is excluded from the advertised modes, so the
// no-quality default must stay on a sustainable, advertised mode.
//
// Bitrate is 14 Mbit/s (aligned with the recorder), NOT the old starved 4 Mbit/s:
// StreamingHandler never sets bitrate_kbps from proto, so this constant is the
// real control point for stream quality. 1080p30 @ 4 Mbit/s looked blocky/
// "phone-like"; ~14 Mbit/s is the quality-path target. Dialable on metal without
// a rebuild via SST_STREAM_BITRATE_KBPS (see rtmp-launch.cpp). A per-stream proto
// field is deferred (see the record/stream quality rework plan, Scope Boundaries).
inline constexpr std::int32_t kDefaultWidth = 1920;
inline constexpr std::int32_t kDefaultHeight = 1080;
inline constexpr std::int32_t kDefaultFramerate = 30;
inline constexpr std::int32_t kDefaultBitrateKbps = 14000;

// In-flight value object passed to a per-destination streamer adapter. The
// streaming service builds this from the app-supplied streaming destination
// (SetStreamingConfig / StreamingControl) at start time. Decouples the
// streaming module from the wire/proto types.
struct PlatformStreamConfig {
    int64_t stream_id{0};
    std::string name;
    PlatformStreamType type{PlatformStreamType::kRtmp};
    std::string url;
    std::string stream_key;
    PlatformStreamCodec codec{PlatformStreamCodec::kH264};
    std::int32_t width{kDefaultWidth};
    std::int32_t height{kDefaultHeight};
    std::int32_t framerate{kDefaultFramerate};
    std::int32_t bitrate_kbps{kDefaultBitrateKbps};
};

}  // namespace sst::streaming
