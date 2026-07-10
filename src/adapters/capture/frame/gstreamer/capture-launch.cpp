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

// Sensor defaults (metal-retuned 2026-07): FULL AUTO. Pinned values render a
// usable image only in the venue they were dialed for (the pinned daylight/
// 1/120s/gain-8 set measured ~28-count means + green cast indoors); the camera
// must work out of the box in ANY venue, so AE and AWB run free by default.
// Auto AWB also measured the two modules' residual casts nearly identical
// (channel ratios within ~4%), while a fixed CCT diverges per module — the
// continuous software auto-WB loop (AutoColorService) then matches both
// cameras to a shared neutral target downstream. The env knobs below still
// pin any dimension for experiments/debugging.
constexpr std::int64_t kDefaultWbMode = 1;  // auto — venue-adaptive AWB
constexpr std::int64_t kWbModeMin = 0;
constexpr std::int64_t kWbModeMax = 9;
constexpr std::int64_t kDefaultExposureNs = 0;  // 0 = AE free (auto exposure)
constexpr double kDefaultGain = 0.0;            // 0 = auto analog gain
constexpr double kDefaultIspDigitalGain = 0.0;  // 0 = auto ISP digital gain

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
    // Sensor readout geometry (selects the Argus 4K mode) vs the delivered
    // working geometry (VIC-downscaled 1080p). See CameraConfig for why they
    // differ — supersampling detail with no CPU-side cost.
    const std::string sensor_width = std::to_string(camera_config.sensor_width);
    const std::string sensor_height = std::to_string(camera_config.sensor_height);
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

    // Downstream frame-rate cap. The sensor runs 4K mode 0 at 30fps (VIC-
    // downscaled to the delivered 1080p in the caps above), so the whole CPU-side
    // pipeline — NV12->BGR postprocess (per camera), record x264, preview x264 and
    // the raw proxy — pays per delivered frame at 30fps. videorate is the belt-and-
    // braces cap (drop-only, never above the sensor rate) in case fps is raised;
    // the expensive stages must never exceed the target rate or every encoder
    // starves (0-byte record, blank preview) on the 6-core Orin Nano. Env-tunable
    // so it can be dialed on-device once headroom is measured.
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
                           isp_tuning + " ! video/x-raw(memory:NVMM),width=" + sensor_width +
                           ",height=" + sensor_height + ",framerate=" + fps + "/1,format=NV12" +
                           " ! nvvidconv"
                           " ! video/x-raw,format=" +
                           format + ",width=" + width + ",height=" + height +
                           " ! videorate drop-only=true max-rate=" + out_fps +
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
