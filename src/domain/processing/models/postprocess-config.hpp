#pragma once

#include <cstdint>

#include "domain/common/models/pixel-format.hpp"

namespace sst::processing {

// Default output resolution: 720p (1280x720).
inline constexpr std::uint32_t kDefaultOutputWidth{1280};
inline constexpr std::uint32_t kDefaultOutputHeight{720};

// Per-channel white-balance correction applied post-demosaic (BGR). The ArduCAM
// RBPCV3 IMX477 ships a heavy magenta cast (green ~40% of R/B) baked into the
// stock Argus ISP tuning; JetPack 7.2 no longer honors the legacy
// `camera_overrides.isp`, and a module-matched `.nito` can't be generated without
// NVIDIA's tuning tooling — so we neutralize downstream with a fixed grey-world
// gain (luma-preserving) on the postprocessed frame. Covers record/stream/preview
// (all pass through the postprocessor); the raw training proxy is uncorrected.
// Defaults measured on-device (R≈B≈2.45×G) — tune here if the cast shifts.
// Grey-world, luma-preserving gains measured on-device (R≈B≈2.45×G magenta).
inline constexpr float kDefaultRGain{0.65F};
inline constexpr float kDefaultGGain{1.59F};
inline constexpr float kDefaultBGain{0.66F};

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
