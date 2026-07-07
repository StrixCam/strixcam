#pragma once

#include <string>

namespace sst::adapters::storage {

// Internal-proxy encode: small H.264 development footage (angle/framing/
// stability/color-grading checks and model training), one pipeline per camera.
// Low bitrate (a development artifact doesn't need broadcast quality) sized to
// be a cheap ADDITIONAL concurrent software encode (no NVENC on the Orin Nano).
inline constexpr int kProxyBitrateKbps = 1500;

// Leaky pre-encoder queue depth (~2 s at 15 fps). Drop-oldest under encode
// overload keeps the proxy realtime so the MP4 finalizes a valid moov, at the
// cost of dropped proxy frames — never a stalled capture thread or a corrupt
// file. The proxy is best-effort development footage; dropping is acceptable.
inline constexpr int kProxyQueueMaxBuffers = 30;

inline constexpr const char* kProxyAppsrcName = "src";
inline constexpr const char* kProxyEncoderName = "enc";

// Builds the per-camera internal-proxy GStreamer launch string: raw NV12 camera
// frames (pushed via the named appsrc at their real geometry) are scaled to the
// internal proxy mode (kProxyVideoMode — 854x480@15, NOT app-controllable) and
// H.264-encoded into `output_mp4`. Pure (no element instantiation) so the shape
// is unit-testable without hardware.
//
// `use_vic` offloads scale + colour-convert to the Jetson VIC (nvvidconv). The
// proxy source is NV12, which nvvidconv accepts natively — no BGRx repack (unlike
// the record/RTMP branches whose source is packed BGR). Software fallback is
// `videoconvert ! videoscale`. `videorate` stays software (VIC does not resample
// framerate). x264enc always receives system-memory I420, behind a
// `leaky=downstream` queue so a slow software encode drops proxy frames instead
// of backing up the appsrc unbounded or corrupting the moov.
[[nodiscard]] auto BuildProxyLaunch(const std::string& output_mp4,
                                    bool use_vic = true) -> std::string;

}  // namespace sst::adapters::storage
