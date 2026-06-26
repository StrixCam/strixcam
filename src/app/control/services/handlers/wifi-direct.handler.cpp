#include "app/control/services/handlers/wifi-direct.handler.hpp"

#include <spdlog/spdlog.h>

#include "domain/streaming/models/app-stream-config.hpp"

namespace sst::control {

namespace {
// Subnet prefix for the group-owner address. The /24 matches the dnsmasq lease
// range (192.168.49.0/24); group_owner_ip carries only the host address.
constexpr const char* kGoSubnetSuffix = "/24";
}  // namespace

WifiDirectHandler::WifiDirectHandler(sst::session::ISessionManager& session, IWifiManager& wifi,
                                     INetworkConfigurator& netcfg, IDhcpServer& dhcp,
                                     sst::streaming::IStreamingService& streaming,
                                     PreviewPort preview_port, DownloadPort download_port)
    : session_(session),
      wifi_(wifi),
      netcfg_(netcfg),
      dhcp_(dhcp),
      streaming_(streaming),
      preview_port_(preview_port.value),
      download_port_(download_port.value) {}

auto WifiDirectHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kStartWifiDirect, sst_cam::Command::kStopWifiDirect};
}

auto WifiDirectHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    if (cmd.payload_case() == sst_cam::Command::kStopWifiDirect) {
        return HandleStop();
    }
    return HandleStart();
}

auto WifiDirectHandler::HandleStart() -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;

    auto group = wifi_.StartP2pGroupOwner();
    if (!group) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("failed to form WiFi Direct group owner");
        return resp;
    }

    // Assign the group-owner IP to the freshly-created P2P interface BEFORE DHCP
    // and RTSP — wpa_supplicant forms the group but never assigns an address, so
    // without this dnsmasq has no subnet and RTSP cannot bind the GO IP.
    if (!netcfg_.AssignGroupOwnerAddress(group->group_interface,
                                         group->group_owner_ip + kGoSubnetSuffix)) {
        wifi_.Stop();
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("failed to assign group-owner IP to the WiFi Direct interface");
        return resp;
    }
    group_interface_ = group->group_interface;

    if (!dhcp_.Start(group->group_interface, group->group_owner_ip)) {
        // The group is up but DHCP failed — tear the group back down so we don't
        // leave a half-open state.
        wifi_.Stop();
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("failed to start DHCP on the WiFi Direct group");
        return resp;
    }

    if (!session_.OnWifiReady()) {
        // SM rejected the transition (e.g. no central connected) — don't report
        // a live group + credentials the session can't actually use. Roll back.
        dhcp_.Stop();
        wifi_.Stop();
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("WiFi Direct group up but session not ready to accept it");
        return resp;
    }

    // Start the RTSP preview now that the group + DHCP are up, bound to the
    // group-owner IP so it is reachable only over the phone link (R22).
    // StopAppStream runs on disconnect via SessionCleanup. A preview failure is
    // degraded-but-not-fatal: the group, credentials, and download path still
    // work, so we log and return the group response rather than rolling back.
    sst::streaming::AppStreamConfig stream_cfg;
    stream_cfg.address = group->group_owner_ip;
    stream_cfg.port = static_cast<std::uint16_t>(preview_port_);
    if (!streaming_.StartAppStream(stream_cfg)) {
        spdlog::warn("WifiDirectHandler: RTSP preview failed to start on {}:{} (preview degraded)",
                     stream_cfg.address, preview_port_);
    }

    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* out = resp.mutable_wifi_direct_group();
    out->set_ssid(group->ssid);
    out->set_psk(group->psk);
    out->set_group_owner_ip(group->group_owner_ip);
    out->set_preview_port(preview_port_);
    out->set_download_port(download_port_);
    out->set_role(group->role);
    return resp;
}

auto WifiDirectHandler::HandleStop() -> sst_cam::CommandResponse {
    dhcp_.Stop();
    // Flush the GO address while the group interface still exists (wifi_.Stop()
    // removes the interface next, which would also drop the address).
    if (!group_interface_.empty()) {
        netcfg_.Clear(group_interface_);
        group_interface_.clear();
    }
    wifi_.Stop();
    session_.OnWifiStopped();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
