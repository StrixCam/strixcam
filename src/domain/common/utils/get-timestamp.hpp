#pragma once

#include <chrono>

#include "domain/common/models/timestamp.hpp"

namespace sst::common::utils {

static inline auto GetCurrentTimestamp() -> sst::common::Timestamp {
    return std::chrono::steady_clock::now();
}
}  // namespace sst::common::utils