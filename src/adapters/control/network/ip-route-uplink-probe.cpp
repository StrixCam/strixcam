#include "adapters/control/network/ip-route-uplink-probe.hpp"

#include <string>

#include "adapters/control/network/subprocess.hpp"

namespace sst::adapters::control {

auto HasDefaultRoute(const std::string& ip_route_default_output) -> bool {
    // `ip route show default` prints one "default via <gw> dev <iface> ..." line
    // per default route, or nothing when there is none. Any "default" token means
    // the camera has an uplink route to the internet.
    return ip_route_default_output.find("default") != std::string::npos;
}

auto IpRouteUplinkProbe::HasInternetUplink() -> bool {
    // Bounded fork/exec (was an unbounded popen): this probe runs on the single
    // BLE dispatcher thread, so a hung `ip` must not stall every BLE command.
    const CaptureResult result = CaptureBounded({"ip", "route", "show", "default"}, kQueryTimeout);
    return HasDefaultRoute(result.output);
}

}  // namespace sst::adapters::control
