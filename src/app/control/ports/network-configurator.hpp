#pragma once

#include <string>

namespace sst::control {

// Assigns the WiFi-Direct group-owner address to the P2P group interface so the
// RTSP preview can bind it and dnsmasq has a subnet to serve. The wpa_supplicant
// group-owner role forms the group but does NOT assign an IP (KTD4 deferred this
// as "deploy-time provisioning"); this port is that step, owned by the firmware.
// On-device this shells out to iproute2 (needs CAP_NET_ADMIN).
class INetworkConfigurator {
   public:
    virtual ~INetworkConfigurator() = default;

    // Assign `cidr` (e.g. "192.168.49.1/24") to `iface` and bring the link up.
    // Returns false if the address assignment failed.
    virtual auto AssignGroupOwnerAddress(const std::string& iface,
                                         const std::string& cidr) -> bool = 0;

    // Best-effort teardown of any address on `iface` (idempotent; errors ignored).
    // The P2P group interface is usually destroyed at group removal anyway.
    virtual auto Clear(const std::string& iface) -> void = 0;
};

}  // namespace sst::control
