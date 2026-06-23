#pragma once

#include <cstdint>

namespace sst::buffer {

enum class BufferPolicy : std::uint8_t {
    LatestOnly,
    DropOldest,
};

}  // namespace sst::buffer
