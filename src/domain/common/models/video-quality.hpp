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
// The fixed-resolution internal dual-camera proxy is deliberately NOT in this
// set — it is not app-controllable (854x480@15, see IProxySink).
//
// 1080p60 is deliberately EXCLUDED: on-device measurement showed software
// x264enc encodes 1080p60 at ~0.64x realtime (no NVENC), so the appsrc backlog
// grows unbounded and the recording never finalizes a valid moov ("no playable
// streams"). 1080p30 and 720p60 both sustain realtime. Do not re-add 1080p60
// without a hardware encoder or a faster encode path.
inline constexpr std::array<VideoQuality, 3> kSupportedVideoModes{{
    {1920, 1080, 30},
    {1280, 720, 60},
    {1280, 720, 30},
}};

// The internal dual-camera proxy encodes at a fixed, low resolution/fps. It is
// NOT app-controllable (the proxy is a firmware-internal development artifact):
// it is deliberately kept OUT of kSupportedVideoModes so it never surfaces in
// DeviceInfoResponse.supported_modes nor passes app record/stream validation.
// The proxy encode builder (see proxy-launch) references this constant directly.
// Kept small + low-fps so the proxy is a cheap ADDITIONAL concurrent x264 encode
// alongside record/preview/stream (VIC scales, but does not raise the encode
// ceiling — see software-h264-encode-ceiling-no-nvenc).
inline constexpr VideoQuality kProxyVideoMode{854, 480, 15};

// True when `quality` is one of the advertised modes exactly. An unset quality
// (all-zero) is not "supported" — callers treat unset and unsupported
// differently (unset → default, unsupported → reject/ignore).
[[nodiscard]] inline auto IsSupportedMode(const VideoQuality& quality) -> bool {
    return std::ranges::any_of(kSupportedVideoModes,
                               [&quality](const VideoQuality& mode) { return mode == quality; });
}

}  // namespace sst::common
