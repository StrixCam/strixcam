#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sst::config {

struct StorageData {
    std::optional<std::string> log{std::nullopt};
    std::optional<std::string> video{std::nullopt};
    std::optional<std::string> snapshots{std::nullopt};
    std::optional<std::string> thumbnails{std::nullopt};
    // Recording starts (full game / event clips) refuse if the SSD has fewer
    // than `min_free_bytes` free at the video root. nullopt disables the check.
    std::optional<std::uint64_t> min_free_bytes{std::nullopt};
    // Bound on total training-proxy storage (the raw__*.mp4 files at the video
    // root). A periodic sweep deletes the oldest complete proxy groups once the
    // total exceeds this, so per-match training footage can't grow unbounded.
    // nullopt disables retention (keep everything).
    std::optional<std::uint64_t> proxy_max_total_bytes{std::nullopt};
};

}  // namespace sst::config
