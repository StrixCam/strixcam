#include <gtest/gtest.h>

#include "adapters/control/network/ip-route-uplink-probe.hpp"

namespace {

using sst::adapters::control::HasDefaultRoute;

TEST(UplinkRouteTest, DefaultRoutePresentMeansUplink) {
    EXPECT_TRUE(HasDefaultRoute("default via 10.10.1.1 dev enP8p1s0 proto dhcp metric 100\n"));
}

TEST(UplinkRouteTest, EmptyOutputMeansNoUplink) { EXPECT_FALSE(HasDefaultRoute("")); }

TEST(UplinkRouteTest, NonDefaultRoutesAreNotAnUplink) {
    // Only link-local / GO subnet routes (no "default" line) — the camera has no
    // path to the internet.
    EXPECT_FALSE(HasDefaultRoute("192.168.49.0/24 dev p2p-wlP1p1s0-0 proto kernel scope link\n"));
}

}  // namespace
