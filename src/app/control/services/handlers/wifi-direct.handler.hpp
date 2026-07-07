#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/control/ports/dhcp-server.hpp"
#include "app/control/ports/handler.hpp"
#include "app/control/ports/network-configurator.hpp"
#include "app/control/ports/wifi-manager.hpp"
#include "app/session/ports/session-manager.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "bluetooth.pb.h"
#include "domain/control/models/service-ports.hpp"
#include "domain/network/models/wifi-direct-group.hpp"

namespace sst::control {

// Handles StartWifiDirect / StopWifiDirect. Start forms a real autonomous P2P
// group owner, brings up DHCP on the group interface, marks the session's WiFi
// group up, starts the RTSP preview stream bound to the group-owner IP, and
// reports the camera-generated credentials in a WifiDirectGroupResponse (KTD4).
// Start is IDEMPOTENT: while the group is already up (an app rejoining a live
// session), it returns the existing credentials without touching
// wpa_supplicant — group re-formation is the known Argus capture killer. Stop
// tears the group + DHCP down (preview teardown runs via session finalize).
class WifiDirectHandler final : public ICommandHandler {
   public:
    WifiDirectHandler(sst::session::ISessionManager& session, IWifiManager& wifi,
                      INetworkConfigurator& netcfg, IDhcpServer& dhcp,
                      sst::streaming::IStreamingService& streaming, PreviewPort preview_port,
                      DownloadPort download_port);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleStart() -> sst_cam::CommandResponse;
    auto HandleStop() -> sst_cam::CommandResponse;
    // The OK response carrying `group`'s credentials + service ports.
    [[nodiscard]] auto BuildGroupResponse(const sst::network::WifiDirectGroup& group) const
        -> sst_cam::CommandResponse;
    // Best-effort RTSP preview start on the group-owner IP; degraded-not-fatal.
    auto EnsurePreviewStarted(const std::string& group_owner_ip) -> void;

    sst::session::ISessionManager& session_;
    IWifiManager& wifi_;
    INetworkConfigurator& netcfg_;
    IDhcpServer& dhcp_;
    sst::streaming::IStreamingService& streaming_;
    std::uint32_t preview_port_;
    std::uint32_t download_port_;
    // Group interface of the active session, retained so HandleStop can flush its
    // address before the group is removed.
    std::string group_interface_;
    // The formed group's credentials, retained for the idempotent rejoin path.
    // Consulted only while wpa_supplicant still reports the group up, so a
    // teardown the handler didn't see (auto-stop finalize) can't serve stale
    // credentials.
    std::optional<sst::network::WifiDirectGroup> active_group_;
};

}  // namespace sst::control
