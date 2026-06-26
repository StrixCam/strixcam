#include "adapters/control/wifi/wpa_supplicant/ip-command.hpp"

#include <string>
#include <vector>

namespace sst::adapters::control {

auto BuildAddrAddArgv(const std::string& cidr,
                      const std::string& iface) -> std::vector<std::string> {
    // A CIDR must carry a prefix length ("/24"); an iface name must be non-empty.
    if (iface.empty() || cidr.find('/') == std::string::npos) {
        return {};
    }
    return {"ip", "addr", "add", cidr, "dev", iface};
}

auto BuildLinkUpArgv(const std::string& iface) -> std::vector<std::string> {
    if (iface.empty()) {
        return {};
    }
    return {"ip", "link", "set", iface, "up"};
}

}  // namespace sst::adapters::control
