#pragma once

#include <string>

#include "app/streaming/ports/uplink-probe.hpp"

namespace sst::adapters::control {

// Pure parser (unit-testable, no I/O): true iff `ip route show default` output
// contains at least one default route. Factored out so the parse is tested
// without touching the routing table. A camera default route implies an active
// uplink — the WiFi-Direct GO is link-local and installs none on the camera.
auto HasDefaultRoute(const std::string& ip_route_default_output) -> bool;

// `ip route show default`-backed uplink probe (hardware-bound — forks `ip`).
// Verified on-device.
class IpRouteUplinkProbe final : public sst::streaming::IUplinkProbe {
   public:
    IpRouteUplinkProbe() = default;
    ~IpRouteUplinkProbe() override = default;

    IpRouteUplinkProbe(const IpRouteUplinkProbe&) = delete;
    auto operator=(const IpRouteUplinkProbe&) -> IpRouteUplinkProbe& = delete;
    IpRouteUplinkProbe(IpRouteUplinkProbe&&) = delete;
    auto operator=(IpRouteUplinkProbe&&) -> IpRouteUplinkProbe& = delete;

    [[nodiscard]] auto HasInternetUplink() -> bool override;
};

}  // namespace sst::adapters::control
