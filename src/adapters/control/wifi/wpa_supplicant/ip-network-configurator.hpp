#pragma once

#include <string>

#include "app/control/ports/network-configurator.hpp"

namespace sst::adapters::control {

// iproute2-backed network configurator (hardware-bound — fork/exec `ip`). Assigns
// the WiFi-Direct group-owner address to the P2P group interface and brings the
// link up; clears it on teardown. Needs CAP_NET_ADMIN (granted to the service via
// the systemd unit's AmbientCapabilities).
class IpNetworkConfigurator final : public sst::control::INetworkConfigurator {
   public:
    IpNetworkConfigurator() = default;
    ~IpNetworkConfigurator() override = default;

    IpNetworkConfigurator(const IpNetworkConfigurator&) = delete;
    auto operator=(const IpNetworkConfigurator&) -> IpNetworkConfigurator& = delete;
    IpNetworkConfigurator(IpNetworkConfigurator&&) = delete;
    auto operator=(IpNetworkConfigurator&&) -> IpNetworkConfigurator& = delete;

    auto AssignGroupOwnerAddress(const std::string& iface,
                                 const std::string& cidr) -> bool override;
    auto Clear(const std::string& iface) -> void override;
};

}  // namespace sst::adapters::control
