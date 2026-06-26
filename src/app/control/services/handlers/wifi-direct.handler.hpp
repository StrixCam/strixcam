#pragma once

#include <cstdint>
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

namespace sst::control {

// Handles StartWifiDirect / StopWifiDirect. Start forms a real autonomous P2P
// group owner, brings up DHCP on the group interface, advances the session to
// WifiReady, starts the RTSP preview stream bound to the group-owner IP, and
// reports the camera-generated credentials in a WifiDirectGroupResponse (KTD4).
// Stop tears the group + DHCP down (preview teardown runs via SessionCleanup on
// disconnect).
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
};

}  // namespace sst::control
