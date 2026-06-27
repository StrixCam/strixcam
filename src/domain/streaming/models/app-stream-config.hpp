#pragma once

#include <cstdint>
#include <string>

#include "domain/processing/models/postprocess-config.hpp"

namespace sst::streaming {

// Always-on RTSP stream the companion app picks up over WiFi-Direct. Consumed
// via appsrc by GstRtspAppStreamServer — the device pushes post-processed
// frames (BGR8) into the server's appsrc, the server encodes with software
// H.264 (x264enc; the Orin Nano has no NVENC) and serves under `mount_point` on
// `port`.
struct AppStreamConfig {
    // The preview pushes the post-processed final frame straight into the appsrc
    // with NO resize (CPU is scarce on the no-NVENC Orin Nano), so the declared
    // caps MUST equal the postprocess output resolution. Tying them to the
    // PostprocessConfig constants prevents drift — a mismatch makes every frame
    // fail the BGR size guard in GstRtspAppStreamServer::Push and the app sticks
    // on "WAITING FOR FRAMES".
    static constexpr std::uint32_t kDefaultWidth = sst::processing::kDefaultOutputWidth;
    static constexpr std::uint32_t kDefaultHeight = sst::processing::kDefaultOutputHeight;
    static constexpr std::uint32_t kDefaultFramerate = 30;
    static constexpr std::uint32_t kDefaultBitrateKbps = 1500;
    static constexpr std::uint16_t kDefaultPort = 8554;

    // Bind address. Defaults to all interfaces; in production the orchestrator
    // sets this to the WiFi Direct group-owner IP so the preview is reachable
    // only over the phone link (R22), never on the cellular interface.
    std::string address{"0.0.0.0"};
    std::string mount_point{"/preview"};
    std::uint16_t port{kDefaultPort};
    std::uint32_t width{kDefaultWidth};
    std::uint32_t height{kDefaultHeight};
    std::uint32_t framerate{kDefaultFramerate};
    std::uint32_t bitrate_kbps{kDefaultBitrateKbps};
};

}  // namespace sst::streaming
