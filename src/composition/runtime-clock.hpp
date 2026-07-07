#pragma once

#include <chrono>
#include <cstdint>

// Composition-root clock sources. Every handler that needs "now" takes it as
// an injected function (testability); these are the production implementations
// main() wires in.
namespace sst::composition {

// Monotonic milliseconds (steady_clock) — for RELATIVE durations only
// (overlay/match clocks). Never decode this as a wall-clock timestamp.
inline auto NowMs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Wall-clock Unix-epoch seconds (download-token TTLs).
inline auto NowUnixSeconds() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// Wall-clock epoch milliseconds. Distinct from NowMs (monotonic steady_clock,
// used for relative overlay/match durations): this is the absolute Unix-epoch
// timestamp the app decodes for MatchState.updated_at and
// ThumbnailResponse.capture_timestamp, so it must come from system_clock.
inline auto NowEpochMs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace sst::composition
