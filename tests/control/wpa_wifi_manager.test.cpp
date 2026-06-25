// Real autonomous P2P group-owner bring-up (U12, R23). Hardware-bound: needs a
// running wpa_supplicant with P2P-GO support — expected to FAIL in the
// container, passes on-device.

#include <gtest/gtest.h>

#include "adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.hpp"

namespace {

TEST(WpaWifiManagerE2E, FormsAutonomousGroupOwner) {
    sst::adapters::control::WpaWifiManager manager("wlan0");
    auto group = manager.StartP2pGroupOwner();
    ASSERT_TRUE(group.has_value()) << "P2P group owner did not form (expected to fail off-device)";
    EXPECT_FALSE(group->ssid.empty());
    EXPECT_FALSE(group->psk.empty());
    EXPECT_EQ(group->group_owner_ip, "192.168.49.1");
    EXPECT_EQ(group->role, "GO");
    manager.Stop();
}

}  // namespace
