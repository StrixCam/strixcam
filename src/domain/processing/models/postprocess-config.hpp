#pragma once

#include <cstdint>

#include "domain/common/models/pixel-format.hpp"

namespace sst::processing {

// Default output resolution: 720p (1280x720).
inline constexpr std::uint32_t kDefaultOutputWidth{1280};
inline constexpr std::uint32_t kDefaultOutputHeight{720};

// Hard-coded defaults. Not loaded from JSON. Override via ctor only in tests.
struct PostprocessConfig {
    std::uint32_t output_width{kDefaultOutputWidth};
    std::uint32_t output_height{kDefaultOutputHeight};
    sst::common::PixelFormat output_format{sst::common::PixelFormat::BGR8};
};

}  // namespace sst::processing
