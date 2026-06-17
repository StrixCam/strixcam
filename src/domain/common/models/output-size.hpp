#pragma once

#include <cstdint>

namespace sst::common {

// Output dimensions for a frame or render target. Plain aggregate — `0` is a
// valid value meaning "keep native size" (the jpeg encoder contract), so there
// is no non-zero invariant to enforce. Carrying width and height together makes
// transposing the two a non-issue: there is only one argument to pass.
struct OutputSize {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

}  // namespace sst::common
