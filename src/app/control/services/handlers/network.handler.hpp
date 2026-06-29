#pragma once

#include <mutex>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/network/ports/uplink-store.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/uplink-config.hpp"

namespace sst::control {

// Handles Set/GetNetworkConfig (U4 / R3, R8): the BLE surface for the camera's
// internet uplink. Set maps the proto config to the domain UplinkData (a FULL
// REPLACE — see uplink-config.hpp), applies it via the UplinkManager (ethernet
// / gated wifi-STA), and — only when the apply succeeded for every ENABLED
// interface — PERSISTS it via the IUplinkStore so it survives restart. The
// response carries live per-interface status and reports ERROR (not OK) when an
// enabled interface failed to come up, so the app does not see a green/OK for a
// config that is actually down. Get returns the current config + live status
// without re-applying. Persistence goes through a port (IUplinkStore) so this
// app-layer handler does not depend on the JSON serde / filesystem (adapters).
class NetworkHandler final : public ICommandHandler {
   public:
    NetworkHandler(sst::network::UplinkManager& manager, sst::network::IUplinkStore& store,
                   sst::config::UplinkData initial);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleSet(const sst_cam::NetworkConfig& proto) -> sst_cam::CommandResponse;
    // Build the response (current config + live per-interface status). `status`
    // sets the ResponseStatus: OK by default, or the caller's error when an
    // enabled interface failed to apply.
    auto BuildResponse(sst_cam::ResponseStatus status) -> sst_cam::CommandResponse;
    auto HandleGet() -> sst_cam::CommandResponse;

    sst::network::UplinkManager& manager_;
    sst::network::IUplinkStore& store_;
    sst::config::UplinkData current_;
    std::mutex mtx_;
};

}  // namespace sst::control
