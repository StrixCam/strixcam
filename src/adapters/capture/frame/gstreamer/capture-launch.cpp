#include "./capture-launch.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

#include "domain/common/models/pixel-format.hpp"
#include "domain/config/utils/parse-model-version.hpp"

namespace sst::capture {

namespace {

auto EnvI64(const char* name, std::int64_t fallback) -> std::int64_t {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

auto EnvDouble(const char* name, double fallback) -> double {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

// Pinned-sensor defaults (metal-tuned 2026-07: both sensors' channel means
// track within a few counts under any pinned config; AE left free diverged
// ~48% between the two modules).
constexpr std::int64_t kDefaultWbMode = 5;  // daylight — fixed CCT, no per-sensor AWB settle
constexpr std::int64_t kWbModeMin = 0;
constexpr std::int64_t kWbModeMax = 9;
constexpr std::int64_t kDefaultExposureNs = 8333333;  // 1/120 s — sports motion
constexpr double kDefaultGain = 8.0;
constexpr double kDefaultIspDigitalGain = 1.0;  // no digital shadow lift — keeps contrast

}  // namespace

auto BuildSensorColorInitFragment() -> std::string {
    // Whole-fragment override wins (same pattern as SST_ISP_TUNING): the
    // rollback/experiment hatch, e.g. SST_CAPTURE_COLOR_INIT="wbmode=1".
    if (const char* whole = std::getenv("SST_CAPTURE_COLOR_INIT"); whole != nullptr) {
        return whole;
    }
    const std::int64_t wbmode =
        std::clamp(EnvI64("SST_CAPTURE_WBMODE", kDefaultWbMode), kWbModeMin, kWbModeMax);
    const std::int64_t exposure_ns = EnvI64("SST_CAPTURE_EXPOSURE_NS", kDefaultExposureNs);
    const double gain = EnvDouble("SST_CAPTURE_GAIN", kDefaultGain);
    const double isp_dgain = EnvDouble("SST_CAPTURE_ISP_DGAIN", kDefaultIspDigitalGain);

    std::string fragment = fmt::format("wbmode={}", wbmode);
    // min==max ranges pin the auto-exposure algorithm to one deterministic
    // point; a 0/negative knob leaves that dimension on auto.
    if (exposure_ns > 0) {
        fragment += fmt::format(" exposuretimerange=\"{} {}\"", exposure_ns, exposure_ns);
    }
    if (gain > 0) {
        fragment += fmt::format(" gainrange=\"{:g} {:g}\"", gain, gain);
    }
    if (isp_dgain > 0) {
        fragment += fmt::format(" ispdigitalgainrange=\"{:g} {:g}\"", isp_dgain, isp_dgain);
    }
    return fragment;
}

auto BuildCaptureLaunch(const CameraConfig& camera_config, const std::string& device_model,
                        std::uint16_t camera_index, const std::string& sink_name) -> std::string {
    const std::string sensor_id = std::to_string(camera_index);
    const std::string width = std::to_string(camera_config.width);
    const std::string height = std::to_string(camera_config.height);
    const std::string fps = std::to_string(camera_config.fps);
    const std::string format = sst::common::ToString(camera_config.format);

    const std::optional<int> model_version = sst::config::ParseModelVersion(device_model);
    if (!model_version) {
        spdlog::error("BuildCaptureLaunch: cannot parse device model '{}'", device_model);
        return {};
    }

    // Deterministic sensor color init: pinned AWB/AE so both sensors boot to
    // the same predetermined values (see capture-launch.hpp for the env knobs).
    const std::string color_init = BuildSensorColorInitFragment();

    // ISP tuning: hardware temporal-noise-reduction + edge-enhancement to fight
    // low-light grain (the ArduCAM module's stock .nito has NR mistuned, and JP7.2
    // blocks retuning). TNR/EE run in the ISP — no CPU cost. Cleaning the noise
    // also sharpens the H.264 output: x264 ultrafast stops spending bits on random
    // noise. One env var overrides the whole fragment so it can be dialed on-device
    // without a rebuild (e.g. lower TNR if fast motion smears).
    const char* isp_env = std::getenv("SST_ISP_TUNING");
    const std::string isp_tuning =
        (isp_env != nullptr) ? isp_env : "tnr-mode=2 tnr-strength=0.5 ee-mode=1 ee-strength=0.4";

    // Downstream frame-rate cap. The IMX477 only offers 1080p at 60fps (no 1080p30
    // sensor mode), so the sensor is driven at camera_config.fps, but the whole
    // CPU-side pipeline — NV12->BGR postprocess (per camera), record x264, preview
    // x264 and the raw proxy — pays per delivered frame. At 60fps x2 cameras that
    // saturates the 6-core Orin Nano and every encoder starves (0-byte record,
    // blank preview). videorate drops to the target rate right after capture so the
    // expensive stages only see 30fps. Env-tunable so it can be raised on-device
    // once headroom is measured; never exceeds the sensor rate.
    constexpr int kDefaultPipelineFps = 30;
    const char* fps_env = std::getenv("SST_PIPELINE_FPS");
    int pipeline_fps = (fps_env != nullptr) ? std::atoi(fps_env) : kDefaultPipelineFps;
    if (pipeline_fps <= 0 || pipeline_fps > camera_config.fps) {
        pipeline_fps = camera_config.fps;
    }
    const std::string out_fps = std::to_string(pipeline_fps);

    std::string gst_pipeline;
    switch (*model_version) {
        case 1:
            gst_pipeline = "nvarguscamerasrc sensor-id=" + sensor_id + " " + color_init + " " +
                           isp_tuning + " ! video/x-raw(memory:NVMM),width=" + width +
                           ",height=" + height + ",framerate=" + fps + "/1,format=NV12" +
                           " ! nvvidconv"
                           " ! video/x-raw,format=" +
                           format + " ! videorate drop-only=true max-rate=" + out_fps +
                           " ! video/x-raw,framerate=" + out_fps + "/1" +
                           " ! appsink name=" + sink_name + " sync=false";
            break;
        default:
            spdlog::error("BuildCaptureLaunch: unsupported device model '{}' (v{})", device_model,
                          *model_version);
            return {};
    }

    spdlog::info("BuildCaptureLaunch: sensor={} model={} format={} {}x{}@{}fps", sensor_id,
                 device_model, format, width, height, fps);
    spdlog::info("BuildCaptureLaunch: pipeline: {}", gst_pipeline);

    return gst_pipeline;
}

}  // namespace sst::capture
