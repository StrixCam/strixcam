#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "app/control/ports/wifi-signal-probe.hpp"
#include "app/control/services/telemetry_probe/telemetry-probe.hpp"
#include "app/streaming/ports/uplink-probe.hpp"

// TelemetryProbe caches background samples into atomics so the BLE dispatcher
// never forks per telemetry request. Driven against fakes (no `ip`/`iw`).

namespace {

using sst::control::TelemetryProbe;

constexpr auto kPollInterval = std::chrono::milliseconds(20);
constexpr auto kSpinStep = std::chrono::milliseconds(5);
constexpr int kRssiStrong = -57;
constexpr int kRssiWeak = -42;

class FakeUplink final : public sst::streaming::IUplinkProbe {
   public:
    std::atomic<bool> value{false};
    std::atomic<int> calls{0};
    [[nodiscard]] auto HasInternetUplink() -> bool override {
        ++calls;
        return value.load();
    }
};

class FakeSignal final : public sst::control::IWifiSignalProbe {
   public:
    std::atomic<int> value{0};  // 0 == "no reading" sentinel for the fake
    std::atomic<int> calls{0};
    [[nodiscard]] auto SampleSignalDbm() -> std::optional<int> override {
        ++calls;
        const int sampled = value.load();
        return sampled == 0 ? std::nullopt : std::optional<int>{sampled};
    }
};

// Spin until `pred` holds or the deadline passes — avoids sleeping a fixed time.
template <typename Pred>
auto WaitFor(Pred pred) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(kSpinStep);
    }
    return pred();
}

TEST(TelemetryProbe, WarmsCacheSynchronouslyOnStart) {
    FakeUplink uplink;
    FakeSignal signal;
    uplink.value = true;
    signal.value = kRssiStrong;

    TelemetryProbe probe(uplink, signal, kPollInterval);
    probe.Start();  // samples once synchronously before returning

    EXPECT_TRUE(probe.InternetReachable());
    const std::optional<int> signal_dbm = probe.WifiSignalDbm();
    ASSERT_TRUE(signal_dbm.has_value());
    EXPECT_EQ(signal_dbm.value_or(0), kRssiStrong);
    EXPECT_GE(uplink.calls.load(), 1);
    EXPECT_GE(signal.calls.load(), 1);
}

TEST(TelemetryProbe, RefreshesCacheOnTheInterval) {
    FakeUplink uplink;
    FakeSignal signal;
    TelemetryProbe probe(uplink, signal, kPollInterval);
    probe.Start();
    EXPECT_FALSE(probe.InternetReachable());
    EXPECT_FALSE(probe.WifiSignalDbm().has_value());

    uplink.value = true;
    signal.value = kRssiWeak;
    EXPECT_TRUE(WaitFor([&] { return probe.InternetReachable(); }));
    EXPECT_TRUE(WaitFor([&] { return probe.WifiSignalDbm() == std::optional<int>{kRssiWeak}; }));
}

TEST(TelemetryProbe, NulloptSignalReportedAsUnknown) {
    FakeUplink uplink;
    FakeSignal signal;  // value 0 → SampleSignalDbm returns nullopt
    TelemetryProbe probe(uplink, signal, kPollInterval);
    probe.Start();
    EXPECT_FALSE(probe.WifiSignalDbm().has_value());
}

TEST(TelemetryProbe, StopIsIdempotentAndJoins) {
    FakeUplink uplink;
    FakeSignal signal;
    TelemetryProbe probe(uplink, signal, kPollInterval);
    probe.Start();
    probe.Stop();
    probe.Stop();  // second Stop is a no-op, must not crash or hang
    SUCCEED();
}

}  // namespace
