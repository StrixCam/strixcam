// IpNetworkConfigurator guard paths (U2). The successful assign forks `ip` and
// needs CAP_NET_ADMIN + a live interface — that is verified on-device, not here.
// These cover the input-validation guards that reject before any exec.

#include <gtest/gtest.h>

#include "adapters/control/wifi/wpa_supplicant/ip-network-configurator.hpp"

namespace {

using sst::adapters::control::IpNetworkConfigurator;

TEST(IpNetworkConfiguratorTest, RejectsEmptyInterfaceWithoutExec) {
    IpNetworkConfigurator cfg;
    EXPECT_FALSE(cfg.AssignGroupOwnerAddress("", "192.168.49.1/24"));
}

TEST(IpNetworkConfiguratorTest, RejectsCidrWithoutPrefixWithoutExec) {
    IpNetworkConfigurator cfg;
    EXPECT_FALSE(cfg.AssignGroupOwnerAddress("p2p-wlP1p1s0-0", "192.168.49.1"));
}

TEST(IpNetworkConfiguratorTest, ClearOnEmptyInterfaceIsNoOp) {
    IpNetworkConfigurator cfg;
    EXPECT_NO_THROW(cfg.Clear(""));
}

}  // namespace
