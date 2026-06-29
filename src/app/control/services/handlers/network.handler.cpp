#include "app/control/services/handlers/network.handler.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace sst::control {

namespace {

// proto NetworkConfig (flat scalars) -> domain UplinkData (optionals). proto3
// scalars are always present, so every field becomes an engaged optional —
// SetNetworkConfig is a FULL REPLACE of the uplink config (see uplink-config.hpp).
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

NetworkHandler::NetworkHandler(sst::network::UplinkManager& manager,
                               sst::network::IUplinkStore& store, sst::config::UplinkData initial)
    : manager_(manager), store_(store), current_(std::move(initial)) {}

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

    // Apply, then GATE on the result: an ENABLED interface whose apply returned
    // ok==false (bad static config, `nmcli con up` rejected, gated wifi) is a
    // real failure. We must NOT reply OK/green and must NOT persist that config
    // as authoritative — otherwise uplink.json would lie about a down interface.
    const auto applied = manager_.Apply(current_);
    const bool ethernet_failed = applied.ethernet_enabled && !applied.ethernet.ok;
    const bool wifi_failed = applied.wifi_enabled && !applied.wifi.ok;

    if (ethernet_failed || wifi_failed) {
        spdlog::error(
            "NetworkHandler: apply failed (ethernet_enabled={} ok={} '{}', wifi_enabled={} ok={} "
            "'{}') — not persisting, reporting ERROR",
            applied.ethernet_enabled, applied.ethernet.ok, applied.ethernet.detail,
            applied.wifi_enabled, applied.wifi.ok, applied.wifi.detail);
        // Report the failure so the app surfaces it instead of a false green.
        return BuildResponse(sst_cam::ResponseStatus::ERROR);
    }

    // Only durable once every enabled interface came up.
    if (!store_.Persist(current_)) {
        spdlog::error("NetworkHandler: uplink applied but persistence failed");
        return BuildResponse(sst_cam::ResponseStatus::ERROR);
    }
    return BuildResponse(sst_cam::ResponseStatus::OK);
}

auto NetworkHandler::BuildResponse(sst_cam::ResponseStatus status) -> sst_cam::CommandResponse {
    // Live interface state (reality), not the last-applied result — the camera's
    // ethernet is NM-managed and may be up even when the uplink config disables it.
    const auto live = manager_.QueryStatus();
    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_network_config();
    UplinkToProto(current_, payload->mutable_config());
    payload->set_ethernet_up(live.ethernet.ok);
    payload->set_ethernet_address(live.ethernet.detail);
    payload->set_wifi_up(live.wifi.ok);
    payload->set_wifi_status(live.wifi.detail);
    resp.set_status(status);
    return resp;
}

auto NetworkHandler::HandleGet() -> sst_cam::CommandResponse {
    return BuildResponse(sst_cam::ResponseStatus::OK);
}

}  // namespace sst::control
