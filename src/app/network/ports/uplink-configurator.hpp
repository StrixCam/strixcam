#pragma once

#include <string>

#include "domain/config/models/uplink-config.hpp"

namespace sst::network {

// Result of applying one uplink interface.
struct UplinkResult {
    bool ok{false};
    std::string detail;  // address on success, or the failure reason
};

// Port for bringing the camera's internet uplink up/down — the cloud-streaming
// path, SEPARATE from the WiFi-Direct GO. Implemented by an nmcli-backed adapter
// (the camera's ethernet + wifi are NetworkManager-managed, unlike the
// firmware-owned P2P radio, so we coordinate with NM rather than fight it).
// Hardware-bound; the real adapter forks `nmcli` and is verified on-device.
class IUplinkConfigurator {
   public:
    IUplinkConfigurator() = default;
    virtual ~IUplinkConfigurator() = default;

    IUplinkConfigurator(const IUplinkConfigurator&) = delete;
    auto operator=(const IUplinkConfigurator&) -> IUplinkConfigurator& = delete;
    IUplinkConfigurator(IUplinkConfigurator&&) = delete;
    auto operator=(IUplinkConfigurator&&) -> IUplinkConfigurator& = delete;

    // Apply the ethernet uplink (DHCP or static) via NM and bring it up.
    virtual auto ApplyEthernet(const sst::config::EthernetUplink& cfg) -> UplinkResult = 0;

    // Apply the WiFi-STA uplink. GATED on single-radio GO+STA concurrency: until
    // that is validated on this hardware, the adapter reports unavailable rather
    // than disturbing the live-preview GO. The camera joins a network as a client.
    virtual auto ApplyWifiSta(const sst::config::WifiStaUplink& cfg) -> UplinkResult = 0;

    // Probe the LIVE state of each interface (not the last-applied config) so the
    // app's Settings -> Network shows reality: the camera's ethernet is
    // NM-managed and may be up with an address even when the firmware uplink
    // config has it disabled. `ok` = the interface currently has connectivity;
    // `detail` = its current address (ethernet) / status (wifi).
    virtual auto ProbeEthernet() -> UplinkResult = 0;
    virtual auto ProbeWifiSta() -> UplinkResult = 0;
};

}  // namespace sst::network
