#pragma once

#include <string>
#include <vector>

#include "domain/config/models/uplink-config.hpp"

namespace sst::adapters::control {

// Pure builders for the `nmcli` argv that configure the camera's ethernet uplink
// connection. Factored out of the fork/exec adapter so argv construction is
// unit-testable without touching NetworkManager (the exec needs a live NM + the
// real connection and is verified on-device). Each returns an empty vector on
// invalid input so the caller fails fast instead of exec'ing a half-formed
// command.

// `nmcli con mod <con> ipv4.method auto` — DHCP. Clears any prior static addr so
// switching static→DHCP doesn't leave a stale manual address pinned.
auto BuildEthDhcpArgv(const std::string& connection) -> std::vector<std::string>;

// `nmcli con mod <con> ipv4.method manual ipv4.addresses <cidr>
//  [ipv4.gateway <gw>] [ipv4.dns <dns>]` — static. `address` must be CIDR
// ("10.0.0.5/24"); gateway/dns are appended only when present.
auto BuildEthStaticArgv(const std::string& connection,
                        const sst::config::EthernetUplink& cfg) -> std::vector<std::string>;

// `nmcli con up <con>` — (re)activate the connection so the modified settings
// take effect.
auto BuildConnectionUpArgv(const std::string& connection) -> std::vector<std::string>;

}  // namespace sst::adapters::control
