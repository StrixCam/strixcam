#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace sst::common {

// A capture/encode mode: resolution + frame rate. The domain twin of the wire
// `VideoQuality` (proto). A zero field means "unset" — the firmware then falls
// back to its per-branch default rather than encoding a 0x0 pipeline. Carrying
// width/height/fps together makes transposing them a non-issue.
struct VideoQuality {
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t fps{0};

    // True only when every field is a positive, usable value. An app that omits
    // the optional wire field decodes to all-zero here → IsSet() == false.
    [[nodiscard]] auto IsSet() const -> bool { return width > 0 && height > 0 && fps > 0; }

    friend auto operator==(const VideoQuality&, const VideoQuality&) -> bool = default;
};

// The concrete record/stream modes the software x264 encode path can deliver on
// the Orin Nano (no NVENC). This is the single source of truth for two things:
//   1. what DeviceInfoResponse.supported_modes advertises to the app, and
//   2. what an app-requested record/stream quality is validated against.
// The fixed-resolution raw dual-recording is deliberately NOT in this set — it
// is not app-controllable (720p, see IRawCaptureSink).
inline constexpr std::array<VideoQuality, 4> kSupportedVideoModes{{
    {1920, 1080, 30},
    {1920, 1080, 60},
    {1280, 720, 30},
    {1280, 720, 60},
}};

// True when `quality` is one of the advertised modes exactly. An unset quality
// (all-zero) is not "supported" — callers treat unset and unsupported
// differently (unset → default, unsupported → reject/ignore).
[[nodiscard]] inline auto IsSupportedMode(const VideoQuality& quality) -> bool {
    return std::ranges::any_of(kSupportedVideoModes,
                               [&quality](const VideoQuality& mode) { return mode == quality; });
}

}  // namespace sst::common
