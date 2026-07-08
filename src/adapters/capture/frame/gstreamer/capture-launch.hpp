#pragma once

#include <cstdint>
#include <string>

#include "domain/capture/models/camera-config.hpp"

namespace sst::capture {

// Per-sensor color init. Default is FULL AUTO (wbmode auto, AE/gain free):
// the camera must render a usable image out of the box in any venue, and
// pinned values only suit the venue they were dialed for. Cross-sensor
// matching is handled downstream by the continuous software auto-WB loop
// (AutoColorService), which pulls both sensors toward one shared neutral
// target — auto AWB leaves the two modules' residual casts nearly identical,
// so the software correction stays small.
//
// Env-tunable (pin any dimension on-device for experiments, no rebuild):
//   SST_CAPTURE_COLOR_INIT  — overrides the WHOLE fragment verbatim (e.g.
//                             "wbmode=5" — fixed-CCT experiment hatch).
//   SST_CAPTURE_WBMODE      — nvarguscamerasrc wbmode enum 0-9 (default 1,
//                             auto; e.g. 5 = daylight fixed CCT).
//   SST_CAPTURE_EXPOSURE_NS — pinned exposure time in ns (default 0 = AE
//                             free; e.g. 8333333 = 1/120 s).
//   SST_CAPTURE_GAIN        — pinned analog gain (default 0 = auto).
//   SST_CAPTURE_ISP_DGAIN   — pinned ISP digital gain (default 0 = auto).
auto BuildSensorColorInitFragment() -> std::string;

// Full nvarguscamerasrc capture launch for one sensor: color-init fragment
// (above) + ISP TNR/EE tuning (SST_ISP_TUNING overrides that fragment) +
// NVMM caps + nvvidconv + videorate cap (SST_PIPELINE_FPS) + named appsink.
// Pure string building — no GStreamer calls — so it is unit-testable without
// hardware. Returns an empty string for an unparseable/unsupported model.
auto BuildCaptureLaunch(const CameraConfig& camera_config, const std::string& device_model,
                        std::uint16_t camera_index, const std::string& sink_name) -> std::string;

}  // namespace sst::capture
