#include "bootstrap/pipeline/color-calibration.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <string>

namespace sst::bootstrap {

namespace {

auto EnvGain(const char* name, float fallback) -> float {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    try {
        return std::stof(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

auto ResolvePostprocessConfig() -> sst::processing::PostprocessConfig {
    sst::processing::PostprocessConfig config;
    auto& white_balance = config.color_correction;
    if (std::getenv("SST_WB_DISABLE") != nullptr) {
        white_balance.enabled = false;
    }
    white_balance.r_gain = EnvGain("SST_WB_RGAIN", white_balance.r_gain);
    white_balance.g_gain = EnvGain("SST_WB_GGAIN", white_balance.g_gain);
    white_balance.b_gain = EnvGain("SST_WB_BGAIN", white_balance.b_gain);
    spdlog::info("Postprocess WB correction: enabled={} R={:.2f} G={:.2f} B={:.2f}",
                 white_balance.enabled, white_balance.r_gain, white_balance.g_gain,
                 white_balance.b_gain);
    return config;
}

auto InitialCalibrationGains(const sst::processing::PostprocessConfig& config)
    -> sst::processing::ColorCalibrationState::Gains {
    const auto& white_balance = config.color_correction;
    return {.r = white_balance.r_gain,
            .g = white_balance.g_gain,
            .b = white_balance.b_gain,
            .enabled = white_balance.enabled,
            .saturation = EnvGain("SST_SATURATION", sst::processing::kDefaultSaturation),
            .contrast = EnvGain("SST_CONTRAST", sst::processing::kDefaultContrast),
            .brightness = EnvGain("SST_BRIGHTNESS", sst::processing::kDefaultBrightness)};
}

}  // namespace sst::bootstrap
