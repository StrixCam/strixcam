#pragma once

#include <chrono>
#include <cstdint>

// Deployment paths + wiring constants for the production composition root.
// Kept in their historical namespaces (sst::paths / sst::runtime_defaults) —
// they are referenced across the composition units, not just main.cpp.
namespace sst::paths {

constexpr const char* kConfigDir = "/etc/sst/cam/config";
constexpr const char* kConfigFormat = "json";
constexpr const char* kVideoRootFallback = "/var/lib/sst/cam/videos";
constexpr const char* kThumbnailRootFallback = "/var/lib/sst/cam/thumbnails";

}  // namespace sst::paths

namespace sst::runtime_defaults {

constexpr std::uint16_t kCamera0Index = 0;
constexpr std::uint16_t kCamera1Index = 1;
constexpr std::uint32_t kOverlayWidth = 1920;  // matches postprocess output (kDefaultOutputWidth)
constexpr std::uint32_t kOverlayHeight = 1080;
constexpr std::uint32_t kPreviewPort = 8554;   // RTSP preview (wifi.proto)
constexpr std::uint32_t kDownloadPort = 8080;  // HTTP downloads
constexpr std::uint64_t kDownloadTokenTtlSeconds = 3600;
constexpr const char* kGroupOwnerIp = "192.168.49.1";
// Bound for the `systemctl reboot` exec — the call returns quickly, but the
// deadline keeps a hung systemd/D-Bus from stalling the dispatcher thread.
constexpr std::chrono::seconds kRebootTimeout{10};
// Bound for the opportunistic `timedatectl set-ntp true` after SetDeviceTime —
// best-effort, so a hung timedated must not stall the dispatcher thread.
constexpr std::chrono::seconds kNtpEnableTimeout{10};

}  // namespace sst::runtime_defaults
