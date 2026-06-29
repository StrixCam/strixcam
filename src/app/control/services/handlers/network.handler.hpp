#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/uplink-config.hpp"

namespace sst::control {

// Handles Set/GetNetworkConfig (U4 / R3, R8): the BLE surface for the camera's
// internet uplink. Set maps the proto config to the domain UplinkData, applies
// it via the UplinkManager (ethernet / gated wifi-STA), PERSISTS it to
// uplink.json so it survives restart, and replies with live per-interface
// status. Get returns the current config + last status without re-applying.
class NetworkHandler final : public ICommandHandler {
   public:
    NetworkHandler(sst::network::UplinkManager& manager, std::string config_path,
                   sst::config::UplinkData initial);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleSet(const sst_cam::NetworkConfig& proto) -> sst_cam::CommandResponse;
    auto HandleGet() -> sst_cam::CommandResponse;
    // Write current_ to config_path_ (best-effort; logs on failure).
    auto Persist() -> void;

    sst::network::UplinkManager& manager_;
    std::string config_path_;
    sst::config::UplinkData current_;
    std::mutex mtx_;
};

}  // namespace sst::control
