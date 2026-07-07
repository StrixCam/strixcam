#include "app/control/services/handlers/device.handler.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "domain/common/models/video-quality.hpp"
#include "domain/control/models/system-stats.hpp"

// Stamped by CMake (git describe). Guarded so the TU still compiles if built
// outside the project's CMake (e.g. a standalone tooling invocation).
#ifndef SST_FIRMWARE_VERSION
#define SST_FIRMWARE_VERSION "unknown"
#endif

namespace sst::control {

namespace {
// Real build version (git describe) — the wire-reported firmware_version. The
// config's `version` field is only a fallback when the build wasn't stamped
// from a git checkout.
constexpr const char* kBuildVersion = SST_FIRMWARE_VERSION;
// Bump on any breaking wire-schema change (proto/README.md versioning policy).
// v2: added the network-config command surface — commands 42 (SetNetworkConfig)
// + 43 (GetNetworkConfig) and response slot 26 (NetworkConfigResponse). New
// capability, not a field addition, so the app gates the Network Settings page
// on protocol_version >= 2 (an older firmware returns UNSUPPORTED with no
// positive signal otherwise).
// v3: RebootCommand (U7) + record/stream quality surface; the app gates the
// Reboot action and quality pickers on protocol_version >= 3.
// v4: state-health cycle — session snapshot / absolute match-state / device
// time command surface (45-47) + per-camera health + DEVICE_INOPERABLE; the
// app runs the reconnect handshake against protocol_version >= 4.
constexpr std::uint32_t kProtocolVersion = 4;
}  // namespace

DeviceHandler::DeviceHandler(sst::config::DeviceData device, ISystemStats& stats,
                             Providers providers)
    : device_(std::move(device)), stats_(stats), providers_(std::move(providers)) {}

auto DeviceHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kGetDeviceInfo, sst_cam::Command::kGetTelemetry};
}

auto DeviceHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    if (cmd.payload_case() == sst_cam::Command::kGetDeviceInfo) {
        return HandleDeviceInfo();
    }
    return HandleTelemetry();
}

auto DeviceHandler::HandleDeviceInfo() const -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* info = resp.mutable_device_info();
    info->set_device_id(device_.serial_number.value_or(""));
    info->set_name(device_.name.value_or(""));
    const std::string build_version = kBuildVersion;
    info->set_firmware_version((build_version.empty() || build_version == "unknown")
                                   ? device_.version.value_or("")
                                   : build_version);
    info->set_model(device_.model.value_or(""));
    info->set_protocol_version(kProtocolVersion);
    // Advertise the concrete record/stream modes the encode pipeline can deliver
    // so the app offers only real options (R16). Single source of truth shared
    // with the record/stream quality validation (kSupportedVideoModes). The
    // fixed-resolution raw dual-recording is deliberately not advertised.
    for (const auto& mode : sst::common::kSupportedVideoModes) {
        auto* wire_mode = info->add_supported_modes();
        wire_mode->set_width(static_cast<std::uint32_t>(mode.width));
        wire_mode->set_height(static_cast<std::uint32_t>(mode.height));
        wire_mode->set_fps(static_cast<std::uint32_t>(mode.fps));
    }
    return resp;
}

auto DeviceHandler::HandleTelemetry() -> sst_cam::CommandResponse {
    const sst::control::SystemStats stats = stats_.Read();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* telemetry = resp.mutable_telemetry();
    telemetry->set_storage_free_bytes(stats.storage_free_bytes);
    telemetry->set_storage_total_bytes(stats.storage_total_bytes);
    telemetry->set_temp_celsius(stats.temp_celsius);
    telemetry->set_ram_used_pct(stats.ram_used_pct);
    telemetry->set_cpu_used_pct(stats.cpu_used_pct);
    telemetry->set_uptime_seconds(stats.uptime_seconds);
    telemetry->set_battery_level_pct(stats.battery_level_pct);
    telemetry->set_is_recording(providers_.is_recording && providers_.is_recording());
    telemetry->set_is_streaming(providers_.is_streaming && providers_.is_streaming());

    // Live WiFi state from the P2P-GO manager (not hardcoded), so the app's wifi
    // indicator matches reality. The camera only ever runs as a WiFi-Direct group
    // owner (no infrastructure join), so "connected" == the GO group is up.
    const sst::control::WifiState wifi =
        providers_.wifi_state ? providers_.wifi_state() : sst::control::WifiState{};
    if (wifi.connected) {
        telemetry->set_wifi_state(sst_cam::WifiState::WIFI_CONNECTED);
        telemetry->set_wifi_ssid(wifi.ssid);
    } else {
        telemetry->set_wifi_state(sst_cam::WifiState::WIFI_DISCONNECTED);
    }
    telemetry->set_internet_reachable(providers_.internet_reachable &&
                                      providers_.internet_reachable());

    // wifi_signal_dbm has no presence bit (plain sint32), and a real RSSI is
    // always negative — so leave it at the proto default 0 to mean "unknown"
    // (no peer / probe unavailable) and only set it when we have a reading.
    if (providers_.wifi_signal_dbm) {
        if (const std::optional<int> dbm = providers_.wifi_signal_dbm()) {
            telemetry->set_wifi_signal_dbm(*dbm);
        }
    }

    // Frame-truth per-camera health (U3). Unset provider / nullopt reading
    // leaves the field absent — "unreported", never a fabricated OK.
    if (providers_.camera0_health) {
        if (const auto health = providers_.camera0_health()) {
            telemetry->set_camera0_health(*health);
        }
    }
    if (providers_.camera1_health) {
        if (const auto health = providers_.camera1_health()) {
            telemetry->set_camera1_health(*health);
        }
    }
    return resp;
}

}  // namespace sst::control
