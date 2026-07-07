// SetDeviceTime handler: phone wall-clock push (state-health cycle U2, proto
// §9b handshake step 2). Pure — clock setter / uplink probe / NTP hook injected.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/set-device-time.handler.hpp"
#include "bluetooth.pb.h"

namespace {

using sst::control::SetDeviceTimeHandler;

// 2020-01-01T00:00:00Z — the handler's plausibility floor (inclusive).
constexpr std::uint64_t kFloorEpochMs = 1'577'836'800'000ULL;
// A plausible phone clock (mid-2025).
constexpr std::uint64_t kPlausibleEpochMs = 1'750'000'000'000ULL;

auto TimeCmd(std::uint64_t epoch_ms) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("time-1");
    cmd.mutable_set_device_time()->set_epoch_ms(epoch_ms);
    return cmd;
}

// Records applications instead of touching the real clock.
struct FakeClock {
    std::optional<std::uint64_t> applied;
    bool ok{true};

    auto Setter() -> SetDeviceTimeHandler::ClockSetter {
        return [this](std::uint64_t epoch_ms) {
            applied = epoch_ms;
            return ok;
        };
    }
};

// Happy path: a plausible epoch is applied and the reply is status-only OK.
TEST(SetDeviceTimeHandlerTest, PlausibleEpochApplied) {
    FakeClock clock;
    SetDeviceTimeHandler handler(clock.Setter(), [] { return false; }, [] {});

    const auto resp = handler.Handle(TimeCmd(kPlausibleEpochMs));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::PAYLOAD_NOT_SET);
    ASSERT_TRUE(clock.applied.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(clock.applied.value(), kPlausibleEpochMs);
}

// Error: a pre-2020 epoch (unset phone clock / garbage) is rejected and the
// system clock is never touched.
TEST(SetDeviceTimeHandlerTest, Pre2020EpochRejectedClockUntouched) {
    FakeClock clock;
    SetDeviceTimeHandler handler(clock.Setter(), [] { return false; }, [] {});

    const auto resp = handler.Handle(TimeCmd(kFloorEpochMs - 1));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(clock.applied.has_value());
}

// Boundary: exactly the 2020-01-01 floor is plausible (inclusive).
TEST(SetDeviceTimeHandlerTest, FloorEpochAccepted) {
    FakeClock clock;
    SetDeviceTimeHandler handler(clock.Setter(), [] { return false; }, [] {});
    EXPECT_EQ(handler.Handle(TimeCmd(kFloorEpochMs)).status(), sst_cam::ResponseStatus::OK);
    EXPECT_TRUE(clock.applied.has_value());
}

// Error: a failing clock set (e.g. missing CAP_SYS_TIME on the device) is
// reported as ERROR, not swallowed as a fake success.
TEST(SetDeviceTimeHandlerTest, ClockSetFailureReported) {
    FakeClock clock;
    clock.ok = false;
    SetDeviceTimeHandler handler(clock.Setter(), [] { return false; }, [] {});
    EXPECT_EQ(handler.Handle(TimeCmd(kPlausibleEpochMs)).status(), sst_cam::ResponseStatus::ERROR);
}

// Opportunistic NTP: enabled only after a successful set AND with a live
// uplink; a rejected epoch or a dead uplink never invokes the hook.
TEST(SetDeviceTimeHandlerTest, NtpHookGatedOnSuccessAndUplink) {
    FakeClock clock;
    int ntp_calls = 0;
    bool uplink = false;
    SetDeviceTimeHandler handler(
        clock.Setter(), [&uplink] { return uplink; }, [&ntp_calls] { ++ntp_calls; });

    // No uplink -> no hook.
    EXPECT_EQ(handler.Handle(TimeCmd(kPlausibleEpochMs)).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(ntp_calls, 0);

    // Uplink up, but the epoch is rejected -> still no hook.
    uplink = true;
    EXPECT_EQ(handler.Handle(TimeCmd(kFloorEpochMs - 1)).status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(ntp_calls, 0);

    // Uplink up + successful set -> hook fires.
    EXPECT_EQ(handler.Handle(TimeCmd(kPlausibleEpochMs)).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(ntp_calls, 1);
}

// Dispatcher parity: set_device_time routes to the handler (correlation echoed).
TEST(SetDeviceTimeHandlerTest, DispatcherRoutesSetDeviceTime) {
    auto clock = std::make_shared<FakeClock>();
    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(
        std::make_shared<SetDeviceTimeHandler>(clock->Setter(), [] { return false; }, [] {}));

    const auto resp = dispatcher.Dispatch(TimeCmd(kPlausibleEpochMs));
    EXPECT_NE(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "time-1");
    EXPECT_TRUE(clock->applied.has_value());
}

}  // namespace
