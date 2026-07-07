#include "app/control/services/handlers/set-device-time.handler.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace sst::control {

namespace {

// Plausibility floor: 2020-01-01T00:00:00Z in epoch milliseconds. Anything
// earlier is an unset phone clock or a corrupt payload — applying it would
// yank device-local timestamps years into the past.
constexpr std::uint64_t kMinPlausibleEpochMs = 1'577'836'800'000ULL;

}  // namespace

SetDeviceTimeHandler::SetDeviceTimeHandler(ClockSetter set_clock, UplinkProbe uplink_active,
                                           NtpEnabler enable_ntp)
    : set_clock_(std::move(set_clock)),
      uplink_active_(std::move(uplink_active)),
      enable_ntp_(std::move(enable_ntp)) {}

auto SetDeviceTimeHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetDeviceTime};
}

auto SetDeviceTimeHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const std::uint64_t epoch_ms = cmd.set_device_time().epoch_ms();

    sst_cam::CommandResponse resp;
    if (epoch_ms < kMinPlausibleEpochMs) {
        spdlog::warn("SetDeviceTime: rejecting implausible epoch {} ms (pre-2020)", epoch_ms);
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("device time rejected: implausible epoch (pre-2020)");
        return resp;
    }
    if (!set_clock_ || !set_clock_(epoch_ms)) {
        spdlog::error("SetDeviceTime: failed to apply epoch {} ms to the system clock", epoch_ms);
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("device time rejected: system clock set failed");
        return resp;
    }
    spdlog::info("SetDeviceTime: system clock set to epoch {} ms", epoch_ms);

    // Opportunistic NTP: only with a live uplink, only best-effort — a hung or
    // denied enable never fails the handshake (the clock is already set).
    if (enable_ntp_ && uplink_active_ && uplink_active_()) {
        enable_ntp_();
    }

    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
