#pragma once

#include <cstdint>

namespace sst::adapters::control {

// Set the system realtime clock (CLOCK_REALTIME) to the given Unix epoch
// milliseconds — the SetDeviceTimeCommand apply mechanism. A direct
// clock_settime(2) call, not a subprocess: it is instantaneous (nothing to
// bound on the dispatcher thread) and fails fast with a real errno. Requires
// CAP_SYS_TIME, granted to the service via the systemd unit's
// AmbientCapabilities (deploy/install.sh); without it the call returns false
// (EPERM, logged) and the clock is untouched.
auto SetRealtimeClock(std::uint64_t epoch_ms) -> bool;

}  // namespace sst::adapters::control
