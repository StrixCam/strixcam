#include "adapters/control/network/nmcli-command.hpp"

#include <string>
#include <vector>

namespace sst::adapters::control {

auto BuildEthDhcpArgv(const std::string& connection) -> std::vector<std::string> {
    if (connection.empty()) {
        return {};
    }
    // Clear any leftover static address when switching to DHCP, else NM keeps the
    // manual ipv4.addresses pinned alongside the auto lease.
    return {"nmcli",          "con", "mod",          connection, "ipv4.method", "auto",
            "ipv4.addresses", "",    "ipv4.gateway", "",         "ipv4.dns",    ""};
}

auto BuildEthStaticArgv(const std::string& connection,
                        const sst::config::EthernetUplink& cfg) -> std::vector<std::string> {
    // A static uplink is meaningless without a CIDR address (must carry "/len").
    const std::string address = cfg.address.value_or("");
    if (connection.empty() || address.find('/') == std::string::npos) {
        return {};
    }
    std::vector<std::string> argv{
        "nmcli", "con", "mod", connection, "ipv4.method", "manual", "ipv4.addresses", address};
    if (cfg.gateway.has_value() && !cfg.gateway->empty()) {
        argv.emplace_back("ipv4.gateway");
        argv.push_back(*cfg.gateway);
    }
    if (cfg.dns.has_value() && !cfg.dns->empty()) {
        argv.emplace_back("ipv4.dns");
        argv.push_back(*cfg.dns);
    }
    return argv;
}

auto BuildConnectionUpArgv(const std::string& connection) -> std::vector<std::string> {
    if (connection.empty()) {
        return {};
    }
    return {"nmcli", "con", "up", connection};
}

}  // namespace sst::adapters::control
