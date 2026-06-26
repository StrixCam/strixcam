#pragma once

#include <string>
#include <vector>

namespace sst::adapters::control {

// Pure builders for the iproute2 commands that bring the WiFi-Direct group-owner
// address up on the P2P group interface. Factored out of the fork/exec adapter so
// the argv construction is unit-testable without touching the network (the exec
// itself needs CAP_NET_ADMIN + a live interface and is verified on-device).
//
// Both return an empty vector when an argument is empty or obviously malformed, so
// the caller fails fast instead of exec'ing a half-formed command.

// `ip addr replace <cidr> dev <iface>` — e.g. ("192.168.49.1/24", "p2p-wlP1p1s0-0").
// `replace` (not `add`) so a GO address left over from a prior session does not
// fail the call. `cidr` must contain a '/' prefix length.
auto BuildAddrReplaceArgv(const std::string& cidr,
                          const std::string& iface) -> std::vector<std::string>;

// `ip link set <iface> up`.
auto BuildLinkUpArgv(const std::string& iface) -> std::vector<std::string>;

}  // namespace sst::adapters::control
