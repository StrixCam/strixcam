#pragma once

#include <cstdint>
#include <string>

#include "domain/capture/models/camera-config.hpp"

namespace sst::capture {

// Deterministic per-sensor color init (pinned AWB/AE). The Argus ISP runs
// per-sensor auto-algorithms (AWB, AE) that settle independently per camera —
// two identical IMX477 modules on the same rig drift to visibly different
// color and brightness (metal-measured: ~48% channel-mean divergence with AE
// free vs <6% pinned). Pinning both sensors to the same predetermined values
// at boot makes them start — and stay — identical; the uniform baked
// postprocess calibration then corrects the module cast on top.
//
// Env-tunable (dial on-device without a rebuild), defaults compiled in:
//   SST_CAPTURE_COLOR_INIT  — overrides the WHOLE fragment verbatim (e.g.
//                             "wbmode=1" restores full auto AWB/AE — rollback).
//   SST_CAPTURE_WBMODE      — nvarguscamerasrc wbmode enum 0-9 (default 5,
//                             daylight: fixed CCT, deterministic; 1 = auto).
//   SST_CAPTURE_EXPOSURE_NS — pinned exposure time in ns (default 8333333 =
//                             1/120 s for sports motion; 0 = leave AE free).
//   SST_CAPTURE_GAIN        — pinned analog gain (default 8; 0 = auto).
//   SST_CAPTURE_ISP_DGAIN   — pinned ISP digital gain (default 1 — no shadow
//                             lift, keeps contrast; 0 = auto).
auto BuildSensorColorInitFragment() -> std::string;

// Full nvarguscamerasrc capture launch for one sensor: color-init fragment
// (above) + ISP TNR/EE tuning (SST_ISP_TUNING overrides that fragment) +
// NVMM caps + nvvidconv + videorate cap (SST_PIPELINE_FPS) + named appsink.
// Pure string building — no GStreamer calls — so it is unit-testable without
// hardware. Returns an empty string for an unparseable/unsupported model.
auto BuildCaptureLaunch(const CameraConfig& camera_config, const std::string& device_model,
                        std::uint16_t camera_index, const std::string& sink_name) -> std::string;

}  // namespace sst::capture
