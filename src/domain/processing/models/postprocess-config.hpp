#pragma once

#include <cstdint>

#include "domain/common/models/pixel-format.hpp"

namespace sst::processing {

// Default output resolution: 720p (1280x720).
inline constexpr std::uint32_t kDefaultOutputWidth{1280};
inline constexpr std::uint32_t kDefaultOutputHeight{720};

// Mild diagonal WB gains (green NOT boosted — a G boost tints the whole frame
// green). Cuts the R/B excess that drives the magenta. Deliberately conservative
// so highlights don't flip green; dial in per scene via SST_WB_{R,G,B}GAIN.
inline constexpr float kDefaultRGain{0.82F};
inline constexpr float kDefaultGGain{1.0F};
inline constexpr float kDefaultBGain{0.84F};

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
