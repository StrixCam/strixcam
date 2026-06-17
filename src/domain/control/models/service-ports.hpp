#pragma once

#include <cstdint>

namespace sst::control {

// Distinct single-field wrappers for the two service ports the WiFi Direct
// handler binds. Both are TCP port numbers (same underlying std::uint32_t), so
// passing them positionally invites a silent transposition. Giving each its own
// type makes a swap a hard compile error and documents intent at the call site.
struct PreviewPort {
    std::uint32_t value{0};
};

struct DownloadPort {
    std::uint32_t value{0};
};

}  // namespace sst::control
