#include "adapters/control/wifi/wpa_supplicant/ip-command.hpp"

#include <string>
#include <vector>

namespace sst::adapters::control {

auto BuildAddrReplaceArgv(const std::string& cidr,
                          const std::string& iface) -> std::vector<std::string> {
    // A CIDR must carry a prefix length ("/24"); an iface name must be non-empty.
    if (iface.empty() || cidr.find('/') == std::string::npos) {
        return {};
    }
    // `replace`, not `add`: the GO address can linger from a prior session whose
    // teardown did not flush it (an abrupt BLE drop skips Clear). `ip addr add`
    // on an already-bound address exits non-zero ("Address already assigned"),
    // which failed the data-plane bring-up; `replace` is idempotent — it adds
    // when absent and is a no-op (success) when already present.
    return {"ip", "addr", "replace", cidr, "dev", iface};
}

auto BuildLinkUpArgv(const std::string& iface) -> std::vector<std::string> {
    if (iface.empty()) {
        return {};
    }
    return {"ip", "link", "set", iface, "up"};
}

}  // namespace sst::adapters::control
