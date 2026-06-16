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
                             FlagProvider is_raw_capturing)
    : device_(std::move(device)),
      stats_(stats),
      is_recording_(std::move(is_recording)),
      is_streaming_(std::move(is_streaming)),
      is_raw_capturing_(std::move(is_raw_capturing)) {}

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
    const sst::control::SystemStats s = stats_.Read();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* t = resp.mutable_telemetry();
    t->set_storage_free_bytes(s.storage_free_bytes);
    t->set_storage_total_bytes(s.storage_total_bytes);
    t->set_temp_celsius(s.temp_celsius);
    t->set_ram_used_pct(s.ram_used_pct);
    t->set_cpu_used_pct(s.cpu_used_pct);
    t->set_uptime_seconds(s.uptime_seconds);
    t->set_battery_level_pct(s.battery_level_pct);
    t->set_is_recording(is_recording_ && is_recording_());
    t->set_is_streaming(is_streaming_ && is_streaming_());
    t->set_is_raw_capturing(is_raw_capturing_ && is_raw_capturing_());

    // WiFi fields stay WIFI_UNKNOWN until the WiFi Direct module reports state
    // (U12); the camera does not currently join an infrastructure network.
    t->set_wifi_state(sst_cam::WifiState::WIFI_UNKNOWN);
    t->set_internet_reachable(false);
    return resp;
}

}  // namespace sst::control
