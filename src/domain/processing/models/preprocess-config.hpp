#pragma once

#include <cstdint>

#include "domain/processing/models/color-mode.hpp"

namespace sst::processing {

// Hard-coded defaults. Not loaded from JSON. Override via ctor only in tests.
inline constexpr std::uint32_t kDefaultAiWidth = 640;
inline constexpr std::uint32_t kDefaultAiHeight = 640;
inline constexpr std::uint8_t kDefaultBinaryThreshold = 127;  // mid-scale for 8-bit luminance

struct PreprocessConfig {
    std::uint32_t ai_width{kDefaultAiWidth};
    std::uint32_t ai_height{kDefaultAiHeight};
    ColorMode ai_color_mode{ColorMode::Grayscale};
    std::uint8_t binary_threshold{kDefaultBinaryThreshold};  // used iff ai_color_mode == Binary
};

}  // namespace sst::processing
