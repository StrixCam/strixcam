#pragma once

#include <cstdint>
#include <string>

namespace sst::streaming {

enum class PlatformStreamType : std::uint8_t { kRtmp = 0, kRtmpS = 1 };
enum class PlatformStreamCodec : std::uint8_t { kH264 = 0, kH265 = 1 };

// Default 1080p60 @ 4 Mbit/s — the platform-stream baseline when the app
// supplies no explicit encode parameters.
inline constexpr std::int32_t kDefaultWidth = 1920;
inline constexpr std::int32_t kDefaultHeight = 1080;
inline constexpr std::int32_t kDefaultFramerate = 60;
inline constexpr std::int32_t kDefaultBitrateKbps = 4000;

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
