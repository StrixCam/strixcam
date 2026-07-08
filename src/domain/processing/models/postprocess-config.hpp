#pragma once

#include <cstdint>

#include "domain/common/models/pixel-format.hpp"

namespace sst::processing {

// Default output resolution: 720p (1280x720).
inline constexpr std::uint32_t kDefaultOutputWidth{1280};
inline constexpr std::uint32_t kDefaultOutputHeight{720};

// Shipping defaults. The R/G/B gains seed NEUTRAL (1.0): the continuous
// auto-WB loop (AutoColorService) owns them at runtime, converging each
// camera toward a shared grey-world target in whatever venue light it boots
// into — a venue-specific baked cast correction would fight it. The tone
// values (dialed on-device against the ArduCAM RBPCV3 IMX477) fix the
// washed-out look and stay applied on top. Override any of them at boot via
// SST_WB_{R,G,B}GAIN / SST_SATURATION / SST_CONTRAST / SST_BRIGHTNESS.
inline constexpr float kDefaultRGain{1.0F};
inline constexpr float kDefaultGGain{1.0F};
inline constexpr float kDefaultBGain{1.0F};
inline constexpr float kDefaultSaturation{1.10F};
inline constexpr float kDefaultContrast{1.20F};
inline constexpr float kDefaultBrightness{-0.05F};

// Per-channel white-balance correction applied post-demosaic (BGR). The ArduCAM
// RBPCV3 IMX477 ships a heavy magenta cast (green ~40% of R/B) baked into the
// stock Argus ISP tuning; JetPack 7.2 no longer honors the legacy
// `camera_overrides.isp`, and a module-matched `.nito` can't be generated without
// NVIDIA's tuning tooling — so we neutralize downstream on the postprocessed
// frame (covers record/stream/preview; the raw training proxy is uncorrected).
//
// This is a DIAGONAL gain: it corrects the average cast but can tint highlights,
// because the true error is a cross-channel CCM. main.cpp exposes
// SST_WB_{R,G,B}GAIN to dial the gains in live on the real (daylight) scene
// without a rebuild; a CCM calibrated against a colour chart is the proper fix.
struct ColorCorrection {
    bool enabled{true};
    float r_gain{kDefaultRGain};
    float g_gain{kDefaultGGain};
    float b_gain{kDefaultBGain};
};

// Hard-coded defaults. Not loaded from JSON. Override via ctor only in tests.
struct PostprocessConfig {
    std::uint32_t output_width{kDefaultOutputWidth};
    std::uint32_t output_height{kDefaultOutputHeight};
    sst::common::PixelFormat output_format{sst::common::PixelFormat::BGR8};
    ColorCorrection color_correction{};
};

}  // namespace sst::processing
