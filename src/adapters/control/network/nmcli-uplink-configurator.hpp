#pragma once

#include <string>

#include "app/network/ports/uplink-configurator.hpp"

namespace sst::adapters::control {

// nmcli-backed uplink configurator (hardware-bound — fork/exec `nmcli`). The
// camera's ethernet + wifi are NetworkManager-managed (only the P2P radio is
// firmware-owned), so the uplink is driven through NM rather than raw `ip`,
// which NM would override. The ethernet connection name + iface are discovered
// at runtime (e.g. "Wired connection 1" / enP8p1s0), not hardcoded. Verified
// on-device.
class NmcliUplinkConfigurator final : public sst::network::IUplinkConfigurator {
   public:
    NmcliUplinkConfigurator() = default;
    ~NmcliUplinkConfigurator() override = default;

    NmcliUplinkConfigurator(const NmcliUplinkConfigurator&) = delete;
    auto operator=(const NmcliUplinkConfigurator&) -> NmcliUplinkConfigurator& = delete;
    NmcliUplinkConfigurator(NmcliUplinkConfigurator&&) = delete;
    auto operator=(NmcliUplinkConfigurator&&) -> NmcliUplinkConfigurator& = delete;

    auto ApplyEthernet(const sst::config::EthernetUplink& cfg)
        -> sst::network::UplinkResult override;
    auto ApplyWifiSta(const sst::config::WifiStaUplink& cfg) -> sst::network::UplinkResult override;
    auto ProbeEthernet() -> sst::network::UplinkResult override;
    auto ProbeWifiSta() -> sst::network::UplinkResult override;

   private:
    // Discover the managed ethernet connection name (empty if none). Runs
    // `nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status`.
    static auto DetectEthernetConnection() -> std::string;
    // Current IPv4 address (CIDR) NM reports for `connection`, empty if none.
    static auto CurrentAddress(const std::string& connection) -> std::string;
};

}  // namespace sst::adapters::control
