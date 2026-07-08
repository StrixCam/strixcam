// Field regression (2026-07-07, real Jetson + phone): one central disconnect
// fires BOTH central-gone signals (GATT StopNotify and Device1
// Connected=false), which produced two unregister+register advertising cycles
// ~15 ms apart; the interleaved async BlueZ calls left the advertisement torn
// down until the 30 s watchdog, so an immediate manual reconnect timed out
// with android error 147 (GATT_CONNECTION_TIMEOUT). The throttle coalesces
// the duplicate: exactly one re-assert per disconnect, while the watchdog
// cadence (30 s) and a later disconnect stay unaffected. Pure — no D-Bus.

#include <gtest/gtest.h>

#include <chrono>

#include "adapters/control/ble/bluez/readvertise-throttle.hpp"

namespace {

using sst::adapters::control::ReAdvertiseThrottle;
using namespace std::chrono_literals;

using Clock = ReAdvertiseThrottle::Clock;

constexpr auto kWindow = 2s;

TEST(ReAdvertiseThrottleTest, FirstAssertAlwaysPasses) {
    ReAdvertiseThrottle throttle{kWindow};
    EXPECT_TRUE(throttle.ShouldAssert(Clock::time_point{}));
}

TEST(ReAdvertiseThrottleTest, DuplicateCentralGoneSignalIsSuppressed) {
    ReAdvertiseThrottle throttle{kWindow};
    const auto now = Clock::now();
    EXPECT_TRUE(throttle.ShouldAssert(now));
    // The second central-gone signal for the SAME disconnect lands ~15 ms later.
    EXPECT_FALSE(throttle.ShouldAssert(now + 15ms));
}

TEST(ReAdvertiseThrottleTest, WatchdogCadencePasses) {
    ReAdvertiseThrottle throttle{kWindow};
    const auto now = Clock::now();
    EXPECT_TRUE(throttle.ShouldAssert(now));
    EXPECT_TRUE(throttle.ShouldAssert(now + 30s));  // next watchdog pass
}

TEST(ReAdvertiseThrottleTest, SuppressedRepeatDoesNotExtendTheWindow) {
    ReAdvertiseThrottle throttle{kWindow};
    const auto now = Clock::now();
    EXPECT_TRUE(throttle.ShouldAssert(now));
    // Suppressed repeats keep arriving just inside the window…
    EXPECT_FALSE(throttle.ShouldAssert(now + 1900ms));
    // …but they must not refresh it: the window is measured from the last
    // ASSERTED re-advertise, so this one (2 s after the assert) passes.
    EXPECT_TRUE(throttle.ShouldAssert(now + 2000ms));
}

TEST(ReAdvertiseThrottleTest, LaterDisconnectPasses) {
    ReAdvertiseThrottle throttle{kWindow};
    const auto now = Clock::now();
    EXPECT_TRUE(throttle.ShouldAssert(now));
    EXPECT_FALSE(throttle.ShouldAssert(now + 15ms));
    // A real subsequent disconnect (seconds later) re-asserts normally.
    EXPECT_TRUE(throttle.ShouldAssert(now + 5s));
}

TEST(ReAdvertiseThrottleTest, ResetForgetsTheLastAssertion) {
    ReAdvertiseThrottle throttle{kWindow};
    const auto now = Clock::now();
    EXPECT_TRUE(throttle.ShouldAssert(now));
    throttle.Reset();
    // Transport restart: the first re-advertise of the new session must never
    // be suppressed by the previous session's timestamp.
    EXPECT_TRUE(throttle.ShouldAssert(now + 1ms));
}

}  // namespace
