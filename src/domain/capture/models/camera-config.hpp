#pragma once

#include <cstdint>

#include "domain/common/models/pixel-format.hpp"

namespace sst::capture {

enum class CameraWhiteBalance : uint8_t { kAuto = 0, kManual = 1 };
enum class CameraFocus : uint8_t { kAuto = 0, kManual = 1 };

// Per-sensor capture configuration for an IMX477. This is device hardware
// config (sensor mode), not business data, so it survives the app-as-source-of-
// truth refactor. Sourced from a compiled-in default today; lens calibration
// lives separately in calibration.json.
struct CameraConfig {
    static constexpr int32_t kDefaultExposure = 100;
    static constexpr float kDefaultGain = 1.0F;
    // The sensor is read out at 4K (IMX477 mode 0, 3840x2160@30) and the VIC
    // downscales to the delivered working resolution (1080p) inside the capture
    // pipeline. Supersampling 4K->1080p is markedly sharper AND lower-noise than
    // the 1080p60 binned mode (mode 1) — measured on metal (~2x real detail).
    // The CPU-side pipeline still only ever sees the delivered 1080p; only the
    // ISP + VIC (hardware) run at 4K.
    static constexpr int32_t kDefaultSensorWidth = 3840;
    static constexpr int32_t kDefaultSensorHeight = 2160;
    // Delivered (working) resolution handed to the appsink / postprocess / encode
    // pipeline — the sensor 4K frame is VIC-downscaled to this before any CPU cost.
    static constexpr int32_t kDefaultWidth = 1920;
    static constexpr int32_t kDefaultHeight = 1080;
    // 4K (mode 0) tops out at 30fps; the pipeline targets 30fps anyway.
    static constexpr int32_t kDefaultFps = 30;

    int32_t exposure{kDefaultExposure};
    float gain{kDefaultGain};
    CameraWhiteBalance white_balance{CameraWhiteBalance::kAuto};
    CameraFocus focus{CameraFocus::kAuto};
    // Sensor readout geometry (selects the Argus sensor mode). Distinct from the
    // delivered width/height: the capture pipeline VIC-downscales sensor ->
    // delivered so supersampling detail is gained with no CPU-side cost.
    int32_t sensor_width{kDefaultSensorWidth};
    int32_t sensor_height{kDefaultSensorHeight};
    int32_t width{kDefaultWidth};
    int32_t height{kDefaultHeight};
    sst::common::PixelFormat format{sst::common::PixelFormat::NV12};
    int32_t fps{kDefaultFps};
};

}  // namespace sst::capture
