#pragma once

#include <chrono>

namespace sst::adapters::control {

// One-assert-per-disconnect guard for the LE advertisement re-registration.
//
// A single central disconnect can fire BOTH central-gone signals (the GATT
// StopNotify callback and the Device1 Connected=false property change), which
// queued two back-to-back unregister+register cycles ~15 ms apart. The second
// cycle's UnregisterAdvertisement races the first cycle's still-in-flight
// async RegisterAdvertisement inside BlueZ and can leave the advertisement
// torn down until the 30 s watchdog restores it — observed on metal
// (2026-07-07) as a manual reconnect dying with android error 147
// (GATT_CONNECTION_TIMEOUT) until the next watchdog pass. One re-assert per
// disconnect is sufficient; repeats inside the suppress window are dropped.
//
// Not thread-safe by design: it is only touched from the transport's single
// disconnect-worker thread.
class ReAdvertiseThrottle {
   public:
    using Clock = std::chrono::steady_clock;

    explicit constexpr ReAdvertiseThrottle(Clock::duration suppress_window)
        : suppress_window_(suppress_window) {}

    // True when a re-advertise should run now; records `now` as the last
    // assertion. False (and no state change — a suppressed repeat must not
    // extend the window) when the last assertion is younger than the window.
    [[nodiscard]] auto ShouldAssert(Clock::time_point now) -> bool {
        if (asserted_ && (now - last_assert_) < suppress_window_) {
            return false;
        }
        asserted_ = true;
        last_assert_ = now;
        return true;
    }

    // Forget the last assertion (transport restart) so the first re-advertise
    // of a new session is never suppressed by a stale timestamp.
    auto Reset() -> void { asserted_ = false; }

   private:
    Clock::duration suppress_window_;
    // Default-constructed time_point is the clock epoch; only read once
    // asserted_ is true, so no explicit init is needed (and clang-tidy's
    // redundant-member-init flags one).
    Clock::time_point last_assert_;
    bool asserted_{false};
};

}  // namespace sst::adapters::control
