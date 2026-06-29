#include "adapters/control/network/ip-route-uplink-probe.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace sst::adapters::control {

namespace {
constexpr std::size_t kReadBufferSize = 256;
}  // namespace

auto HasDefaultRoute(const std::string& ip_route_default_output) -> bool {
    // `ip route show default` prints one "default via <gw> dev <iface> ..." line
    // per default route, or nothing when there is none. Any "default" token means
    // the camera has an uplink route to the internet.
    return ip_route_default_output.find("default") != std::string::npos;
}

auto IpRouteUplinkProbe::HasInternetUplink() -> bool {
    std::array<char, kReadBufferSize> buffer{};
    std::string out;
    // NOLINTNEXTLINE(cert-env33-c) — fixed command string, no user input.
    FILE* pipe = ::popen("ip route show default", "r");
    if (pipe == nullptr) {
        return false;
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
    ::pclose(pipe);
    return HasDefaultRoute(out);
}

}  // namespace sst::adapters::control
