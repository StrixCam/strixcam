// Device-info + telemetry handlers (U7, R7/R8).
//
// Pure — handler driven against a fake ISystemStats; no hardware sensor reads.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

#include "app/control/ports/system-stats.hpp"
#include "app/control/ports/wifi-manager.hpp"
#include "app/control/services/handlers/device.handler.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/device.hpp"

// Same git-describe stamp the handler reports; guarded for non-CMake builds.
#ifndef SST_FIRMWARE_VERSION
#define SST_FIRMWARE_VERSION "unknown"
#endif

namespace {

using sst::control::DeviceHandler;

// Default wifi-state provider: disconnected (no P2P group). Individual tests
// override with a connected state.
auto NoWifi() -> sst::control::WifiState { return sst::control::WifiState{}; }

// Default signal provider: no connected peer → nullopt → wifi_signal_dbm unset.
auto NoSignal() -> std::optional<int> { return std::nullopt; }

// Fixed telemetry values the fake reports; the tests assert these exact numbers
// flow through to the response.
constexpr std::uint64_t kStorageFreeBytes = 1000;
constexpr std::uint64_t kStorageTotalBytes = 4000;
constexpr std::uint64_t kUptimeSeconds = 12345;
constexpr float kCpuUsedPct = 25.0F;
constexpr int kPeerRssiDbm = -57;

class FakeStats final : public sst::control::ISystemStats {
   public:
    [[nodiscard]] auto Read() const -> sst::control::SystemStats override {
        ++reads;
        sst::control::SystemStats stats;
        stats.storage_free_bytes = kStorageFreeBytes;
        stats.storage_total_bytes = kStorageTotalBytes;
        stats.uptime_seconds = kUptimeSeconds;
        stats.cpu_used_pct = kCpuUsedPct;
        return stats;
    }
    mutable int reads{0};
};

auto MakeDevice() -> sst::config::DeviceData {
    sst::config::DeviceData device;
    device.name = "sst-cam";
    device.model = "v1";
    device.version = "1.0.0";
    device.serial_number = "00000042";
    return device;
}

auto DeviceInfoCommand() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-info");
    cmd.mutable_get_device_info();
    return cmd;
}

auto TelemetryCommand() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-tel");
    cmd.mutable_get_telemetry();
    return cmd;
}

// R8: GetDeviceInfo returns the configured identity + a non-zero protocol_version.
// A flat sequence of EXPECT_* assertions; the cognitive-complexity score is
// inflated by gtest macro expansion, and splitting the spec-style assertion list
// into helpers would hurt readability — hence the suppression on the next line.
TEST(DeviceHandlerTest,  // NOLINT(readability-function-cognitive-complexity)
     DeviceInfoReturnsIdentityAndProtocolVersion) {
    FakeStats stats;
    DeviceHandler handler(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, NoSignal);

    auto resp = handler.Handle(DeviceInfoCommand());

    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kDeviceInfo);
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.device_info().device_id(), "00000042");
    EXPECT_EQ(resp.device_info().name(), "sst-cam");
    EXPECT_EQ(resp.device_info().model(), "v1");
    // firmware_version is the real build (git describe), not the config's
    // "1.0.0" placeholder — mirror the handler's fallback so this holds whether
    // or not the test build was stamped from a git checkout.
    const std::string stamped = SST_FIRMWARE_VERSION;
    const std::string expected_fw = (stamped.empty() || stamped == "unknown") ? "1.0.0" : stamped;
    EXPECT_EQ(resp.device_info().firmware_version(), expected_fw);
    // The reboot command surface (U7) bumped kProtocolVersion to 3; the app gates
    // Reboot on an exact version match, so guard against a downward regression.
    EXPECT_GE(resp.device_info().protocol_version(), 3U);
}

// is_recording / is_streaming / is_raw_capturing reflect the injected providers,
// each independently (is_raw_capturing is wire field 14, set independently of
// is_recording — the app reads it to show raw-capture running state).
// A flat sequence of EXPECT_* assertions; the cognitive-complexity score is
// inflated by gtest macro expansion, and splitting the spec-style assertion list
// into helpers would hurt readability — hence the suppression on the next line.
TEST(DeviceHandlerTest,  // NOLINT(readability-function-cognitive-complexity)
     TelemetryReflectsRecordingStreamingAndRawCapturingFlags) {
    FakeStats stats;
    DeviceHandler handler(
        MakeDevice(), stats, [] { return true; }, [] { return false; }, [] { return true; }, NoWifi,
        [] { return true; }, NoSignal);

    auto resp = handler.Handle(TelemetryCommand());

    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kTelemetry);
    EXPECT_EQ(resp.telemetry().storage_free_bytes(), kStorageFreeBytes);
    EXPECT_EQ(resp.telemetry().storage_total_bytes(), kStorageTotalBytes);
    EXPECT_EQ(resp.telemetry().uptime_seconds(), kUptimeSeconds);
    EXPECT_TRUE(resp.telemetry().is_recording());
    EXPECT_FALSE(resp.telemetry().is_streaming());
    // Set independently of is_recording (which is true here): proves field 14 is
    // wired, not mirroring is_recording.
    EXPECT_TRUE(resp.telemetry().is_raw_capturing());
    // internet_reachable reflects the injected uplink probe (true here), not a
    // hardcoded false.
    EXPECT_TRUE(resp.telemetry().internet_reachable());
}

// The internet_reachable flag tracks the injected uplink probe in both
// directions: a false-returning probe must report false (it replaced a
// hardcoded false, so guard the live false path too, not just the true case).
TEST(DeviceHandlerTest, TelemetryInternetReachableReflectsFalseProbe) {
    FakeStats stats;
    DeviceHandler handler(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, NoSignal);

    auto resp = handler.Handle(TelemetryCommand());

    EXPECT_FALSE(resp.telemetry().internet_reachable());
}

// U9.2: wifi_signal_dbm carries the peer RSSI when the probe has a reading, and
// stays at the proto default 0 ("unknown") when the probe returns nullopt — the
// app distinguishes the two (real RSSI is always negative).
TEST(DeviceHandlerTest, TelemetryWifiSignalDbmReflectsProbe) {
    FakeStats stats;
    DeviceHandler with_signal(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, [] { return std::optional<int>{kPeerRssiDbm}; });
    EXPECT_EQ(with_signal.Handle(TelemetryCommand()).telemetry().wifi_signal_dbm(), kPeerRssiDbm);

    DeviceHandler no_signal(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, NoSignal);
    EXPECT_EQ(no_signal.Handle(TelemetryCommand()).telemetry().wifi_signal_dbm(), 0);
}

// R7: the handler never reads stats / produces telemetry unless a command is
// dispatched — no background polling, no unsolicited push.
TEST(DeviceHandlerTest, NoTelemetryWithoutACommand) {
    FakeStats stats;
    DeviceHandler handler(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, NoSignal);

    EXPECT_EQ(stats.reads, 0);  // nothing read until asked

    handler.Handle(TelemetryCommand());
    EXPECT_EQ(stats.reads, 1);  // exactly one read per GetTelemetry
}

// Telemetry reports the LIVE wifi state (not a hardcoded UNKNOWN): a connected
// P2P-GO maps to WIFI_CONNECTED + ssid; no group maps to WIFI_DISCONNECTED. This
// keeps the app's wifi indicator coherent with the actual preview link.
TEST(DeviceHandlerTest, TelemetryReportsLiveWifiState) {
    FakeStats stats;
    DeviceHandler connected_handler(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        [] {
            return sst::control::WifiState{.mode = sst::control::WifiMode::kP2pGroupOwner,
                                           .connected = true,
                                           .ssid = "DIRECT-sst-cam",
                                           .ip_address = "192.168.49.1"};
        },
        [] { return false; }, NoSignal);
    auto connected = connected_handler.Handle(TelemetryCommand());
    EXPECT_EQ(connected.telemetry().wifi_state(), sst_cam::WifiState::WIFI_CONNECTED);
    EXPECT_EQ(connected.telemetry().wifi_ssid(), "DIRECT-sst-cam");

    DeviceHandler off_handler(
        MakeDevice(), stats, [] { return false; }, [] { return false; }, [] { return false; },
        NoWifi, [] { return false; }, NoSignal);
    auto off = off_handler.Handle(TelemetryCommand());
    EXPECT_EQ(off.telemetry().wifi_state(), sst_cam::WifiState::WIFI_DISCONNECTED);
}

}  // namespace
