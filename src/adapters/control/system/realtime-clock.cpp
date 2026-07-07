#include "adapters/control/system/realtime-clock.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace sst::adapters::control {

namespace {

constexpr std::uint64_t kMillisPerSecond = 1000ULL;
constexpr std::uint64_t kNanosPerMilli = 1'000'000ULL;

}  // namespace

auto SetRealtimeClock(std::uint64_t epoch_ms) -> bool {
    timespec spec{};
    spec.tv_sec = static_cast<time_t>(epoch_ms / kMillisPerSecond);
    // tv_nsec's type is POSIX-pinned (long on this ABI) — cast to whatever the
    // struct declares rather than a fixed-width type.
    spec.tv_nsec =
        static_cast<decltype(spec.tv_nsec)>((epoch_ms % kMillisPerSecond) * kNanosPerMilli);
    if (clock_settime(CLOCK_REALTIME, &spec) != 0) {
        const int err = errno;
        spdlog::error("SetRealtimeClock: clock_settime failed: {} (CAP_SYS_TIME missing?)",
                      std::strerror(err));
        return false;
    }
    return true;
}

}  // namespace sst::adapters::control
