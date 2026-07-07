#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles SetDeviceTimeCommand — the phone pushes its wall clock right after
// the protocol gate on every connect (proto §9b handshake step 2). Applies the
// epoch to the SYSTEM clock only (file mtimes, summary fields, logs); session
// and match clocks on the wire stay monotonic/relative. Implausible values
// (pre-2020 epoch — an unset phone clock or a garbage payload) are rejected
// with ERROR and the clock is left untouched. Reply is status-only.
//
// After a successful set, when an internet uplink is active, the optional NTP
// hook is invoked best-effort (opportunistic NTP keeps the clock honest for
// the rest of the session; its failure never fails the command).
class SetDeviceTimeHandler final : public ICommandHandler {
   public:
    // Applies epoch milliseconds to the system realtime clock. Returns false
    // when the set failed (e.g. the process lacks CAP_SYS_TIME).
    using ClockSetter = std::function<bool(std::uint64_t epoch_ms)>;
    // True when the camera has an internet uplink (same source as telemetry's
    // internet_reachable). Unset -> the NTP hook is never invoked.
    using UplinkProbe = std::function<bool()>;
    // Best-effort NTP enable (bounded subprocess in production wiring).
    using NtpEnabler = std::function<void()>;

    SetDeviceTimeHandler(ClockSetter set_clock, UplinkProbe uplink_active, NtpEnabler enable_ntp);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    ClockSetter set_clock_;
    UplinkProbe uplink_active_;
    NtpEnabler enable_ntp_;
};

}  // namespace sst::control
