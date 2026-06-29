#include "app/control/services/handlers/network.handler.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

#include "adapters/config/json/serde/uplink-config.hpp"  // to_json for persistence

namespace sst::control {

namespace {

// proto NetworkConfig (flat scalars) -> domain UplinkData (optionals). proto3
// scalars are always present, so every field becomes an engaged optional.
auto ProtoToUplink(const sst_cam::NetworkConfig& proto) -> sst::config::UplinkData {
    sst::config::UplinkData out;
    const auto& eth = proto.ethernet();
    out.ethernet.enabled = eth.enabled();
    out.ethernet.dhcp = eth.dhcp();
    out.ethernet.address = eth.address();
    out.ethernet.gateway = eth.gateway();
    out.ethernet.dns = eth.dns();
    const auto& wifi = proto.wifi();
    out.wifi.enabled = wifi.enabled();
    out.wifi.ssid = wifi.ssid();
    out.wifi.passphrase = wifi.password();
    out.wifi.dhcp = wifi.dhcp();
    out.wifi.address = wifi.address();
    out.wifi.gateway = wifi.gateway();
    out.wifi.dns = wifi.dns();
    return out;
}

// domain UplinkData -> proto NetworkConfig (unset optional -> proto default).
void UplinkToProto(const sst::config::UplinkData& data, sst_cam::NetworkConfig* proto) {
    auto* eth = proto->mutable_ethernet();
    eth->set_enabled(data.ethernet.enabled.value_or(false));
    eth->set_dhcp(data.ethernet.dhcp.value_or(true));
    eth->set_address(data.ethernet.address.value_or(""));
    eth->set_gateway(data.ethernet.gateway.value_or(""));
    eth->set_dns(data.ethernet.dns.value_or(""));
    auto* wifi = proto->mutable_wifi();
    wifi->set_enabled(data.wifi.enabled.value_or(false));
    wifi->set_ssid(data.wifi.ssid.value_or(""));
    wifi->set_password(data.wifi.passphrase.value_or(""));
    wifi->set_dhcp(data.wifi.dhcp.value_or(true));
    wifi->set_address(data.wifi.address.value_or(""));
    wifi->set_gateway(data.wifi.gateway.value_or(""));
    wifi->set_dns(data.wifi.dns.value_or(""));
}

}  // namespace

NetworkHandler::NetworkHandler(sst::network::UplinkManager& manager, std::string config_path,
                               sst::config::UplinkData initial)
    : manager_(manager), config_path_(std::move(config_path)), current_(std::move(initial)) {}

auto NetworkHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetNetworkConfig, sst_cam::Command::kGetNetworkConfig};
}

auto NetworkHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    if (cmd.payload_case() == sst_cam::Command::kSetNetworkConfig) {
        return HandleSet(cmd.set_network_config().config());
    }
    return HandleGet();
}

auto NetworkHandler::HandleSet(const sst_cam::NetworkConfig& proto) -> sst_cam::CommandResponse {
    std::lock_guard lock(mtx_);
    current_ = ProtoToUplink(proto);
    manager_.Apply(current_);
    Persist();
    return HandleGet();  // echo back the applied config + fresh LIVE status
}

auto NetworkHandler::HandleGet() -> sst_cam::CommandResponse {
    // Live interface state (reality), not the last-applied result — the camera's
    // ethernet is NM-managed and may be up even when the uplink config disables it.
    const auto status = manager_.QueryStatus();
    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_network_config();
    UplinkToProto(current_, payload->mutable_config());
    payload->set_ethernet_up(status.ethernet.ok);
    payload->set_ethernet_address(status.ethernet.detail);
    payload->set_wifi_up(status.wifi.ok);
    payload->set_wifi_status(status.wifi.detail);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

auto NetworkHandler::Persist() -> void {
    const nlohmann::json json_config = current_;
    std::ofstream out(config_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("NetworkHandler: could not persist uplink config to {}", config_path_);
        return;
    }
    out << json_config.dump(2) << '\n';
    spdlog::info("NetworkHandler: persisted uplink config to {}", config_path_);
}

}  // namespace sst::control
