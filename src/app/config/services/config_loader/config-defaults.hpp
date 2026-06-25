#pragma once

#include <string_view>

// Built-in default configuration, written verbatim to the config dir when a file
// is missing (first run on a fresh device). Authored as JSON — the same schema
// the json serde already parses — so no per-struct serializer is needed. These
// are placeholders meant to let the firmware start out-of-box; the companion app
// provisions real device identity / calibration over the control plane, and the
// files are plain JSON the operator can edit.
namespace sst::config::defaults {

inline constexpr std::string_view kDeviceJson = R"({
  "manufacturer": "Scout Sport Technology",
  "model": "v1",
  "name": "sst-cam",
  "serial_number": "00000000",
  "timestamp": "monotonic",
  "timezone": "UTC",
  "version": "1.0.0"
}
)";

inline constexpr std::string_view kCalibrationJson = R"({
  "cameras": {
    "exposure": 100,
    "gain": 1.0,
    "white_balance": 0,
    "focus": 0,
    "width": 1920,
    "height": 1080,
    "format": 0,
    "fps": 60,
    "device": [
      {
        "id": 0,
        "intrinsic_matrix": [1000.0, 0.0, 960.0, 0.0, 1000.0, 540.0, 0.0, 0.0, 1.0],
        "distortion_coefficients": [0.0, 0.0, 0.0, 0.0, 0.0]
      },
      {
        "id": 1,
        "intrinsic_matrix": [1000.0, 0.0, 960.0, 0.0, 1000.0, 540.0, 0.0, 0.0, 1.0],
        "distortion_coefficients": [0.0, 0.0, 0.0, 0.0, 0.0]
      }
    ]
  },
  "microphones": {
    "noise_reduction": true,
    "device": [
      {"id": 0, "sensitivity": 1.0},
      {"id": 1, "sensitivity": 1.0}
    ]
  }
}
)";

inline constexpr std::string_view kStorageJson = R"({
  "log": "/var/log/sst/cam/",
  "video": "/var/lib/sst/cam/videos/",
  "snapshots": "/var/lib/sst/cam/snapshots/",
  "thumbnails": "/var/lib/sst/cam/thumbnails/"
}
)";

inline constexpr std::string_view kWifiDirectJson = R"({
  "enabled": true,
  "ssid": "DIRECT-SST-CAM",
  "passphrase": "scoutcam1234",
  "channel": 6,
  "ip_address": "192.168.49.1"
}
)";

}  // namespace sst::config::defaults
