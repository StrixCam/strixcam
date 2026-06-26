// Pure iproute2 argv builders (U1). No network — argv construction only.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "adapters/control/wifi/wpa_supplicant/ip-command.hpp"

namespace {

using sst::adapters::control::BuildAddrReplaceArgv;
using sst::adapters::control::BuildLinkUpArgv;
using Argv = std::vector<std::string>;

TEST(IpCommandTest, BuildsAddrReplaceArgv) {
    // `replace` (not `add`) so a lingering GO address from a prior session is a
    // no-op instead of an "Address already assigned" failure.
    EXPECT_EQ(BuildAddrReplaceArgv("192.168.49.1/24", "p2p-wlP1p1s0-0"),
              (Argv{"ip", "addr", "replace", "192.168.49.1/24", "dev", "p2p-wlP1p1s0-0"}));
}

TEST(IpCommandTest, BuildsLinkUpArgv) {
    EXPECT_EQ(BuildLinkUpArgv("p2p-wlP1p1s0-0"),
              (Argv{"ip", "link", "set", "p2p-wlP1p1s0-0", "up"}));
}

TEST(IpCommandTest, AddrAddRejectsEmptyIface) {
    EXPECT_TRUE(BuildAddrReplaceArgv("192.168.49.1/24", "").empty());
}

TEST(IpCommandTest, AddrAddRejectsCidrWithoutPrefix) {
    // No '/' prefix length — a bare address is not a valid `ip addr add` target.
    EXPECT_TRUE(BuildAddrReplaceArgv("192.168.49.1", "p2p-wlP1p1s0-0").empty());
}

TEST(IpCommandTest, LinkUpRejectsEmptyIface) { EXPECT_TRUE(BuildLinkUpArgv("").empty()); }

}  // namespace
