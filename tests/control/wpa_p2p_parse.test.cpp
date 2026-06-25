// wpa_supplicant P2P event parsing (U12, R23). Pure — faked ctrl_iface text.

#include <gtest/gtest.h>

#include "adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp"

namespace {

using sst::adapters::control::ParseGroupInterface;
using sst::adapters::control::ParseGroupStarted;
using sst::adapters::control::ParseQuotedField;

TEST(WpaP2pParseTest, ParsesGroupStartedEvent) {
    const std::string event =
        "<3>P2P-GROUP-STARTED p2p-wlan0-0 GO ssid=\"DIRECT-Ab-ScoutCam\" freq=2412 "
        "passphrase=\"swXY1234\" go_dev_addr=02:11:22:33:44:55";

    auto parsed = ParseGroupStarted(event);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->interface, "p2p-wlan0-0");
    EXPECT_EQ(parsed->ssid, "DIRECT-Ab-ScoutCam");
    EXPECT_EQ(parsed->passphrase, "swXY1234");
}

TEST(WpaP2pParseTest, ParseQuotedFieldAndInterface) {
    EXPECT_EQ(ParseQuotedField("a ssid=\"hi there\" b", "ssid").value_or(""), "hi there");
    EXPECT_FALSE(ParseQuotedField("no field here", "ssid").has_value());
    EXPECT_EQ(ParseGroupInterface("P2P-GROUP-STARTED p2p-wlan0-3 GO").value_or(""), "p2p-wlan0-3");
}

TEST(WpaP2pParseTest, IncompleteEventRejected) {
    // Missing passphrase -> no parse.
    EXPECT_FALSE(ParseGroupStarted("P2P-GROUP-STARTED p2p-wlan0-0 GO ssid=\"x\"").has_value());
}

}  // namespace
