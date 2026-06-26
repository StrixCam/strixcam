#include "app/control/services/handlers/device.handler.hpp"

#include <cstdint>
#include <utility>

#include "domain/control/models/system-stats.hpp"

namespace sst::control {

namespace {
// Bump on any breaking wire-schema change (proto/README.md versioning policy).
constexpr std::uint32_t kProtocolVersion = 1;
}  // namespace

DeviceHandler::DeviceHandler(sst::config::DeviceData device, ISystemStats& stats,
                             FlagProvider is_recording, FlagProvider is_streaming,
                             FlagProvider is_raw_capturing, WifiStateProvider wifi_state)
    : device_(std::move(device)),
      stats_(stats),
      is_recording_(std::move(is_recording)),
      is_streaming_(std::move(is_streaming)),
      is_raw_capturing_(std::move(is_raw_capturing)),
      wifi_state_(std::move(wifi_state)) {}

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
    info->set_firmware_version(device_.version.value_or(""));
    info->set_model(device_.model.value_or(""));
    info->set_protocol_version(kProtocolVersion);
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
    telemetry->set_is_recording(is_recording_ && is_recording_());
    telemetry->set_is_streaming(is_streaming_ && is_streaming_());
    telemetry->set_is_raw_capturing(is_raw_capturing_ && is_raw_capturing_());

    // Live WiFi state from the P2P-GO manager (not hardcoded), so the app's wifi
    // indicator matches reality. The camera only ever runs as a WiFi-Direct group
    // owner (no infrastructure join), so "connected" == the GO group is up.
    const sst::control::WifiState wifi = wifi_state_ ? wifi_state_() : sst::control::WifiState{};
    if (wifi.connected) {
        telemetry->set_wifi_state(sst_cam::WifiState::WIFI_CONNECTED);
        telemetry->set_wifi_ssid(wifi.ssid);
    } else {
        telemetry->set_wifi_state(sst_cam::WifiState::WIFI_DISCONNECTED);
    }
    telemetry->set_internet_reachable(false);
    return resp;
}

}  // namespace sst::control
