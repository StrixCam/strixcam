#include <gtest/gtest.h>

#include <string>

#include "adapters/control/network/iw-station-rssi-probe.hpp"

// Pure parser tests against real `iw` output shapes captured on the Jetson GO.

namespace {

using sst::adapters::control::ParseP2pGoInterface;
using sst::adapters::control::ParseStationSignalDbm;

constexpr const char* kIwDevP2pGo =
    "phy#0\n"
    "\tInterface wlP1p1s0\n"
    "\t\tifindex 2\n"
    "\t\taddr f8:3d:c6:1f:8c:71\n"
    "\t\tssid DIRECT-s2-sst-cam-7967\n"
    "\t\ttype P2P-GO\n"
    "\t\tchannel 11 (2462 MHz)\n";

TEST(ParseP2pGoInterface, FindsTheGroupOwnerInterface) {
    const auto iface = ParseP2pGoInterface(kIwDevP2pGo);
    ASSERT_TRUE(iface.has_value());
    EXPECT_EQ(iface.value_or(""), "wlP1p1s0");
}

TEST(ParseP2pGoInterface, PicksTheGoAmongSeveralInterfaces) {
    const std::string out =
        "\tInterface eth-mon\n\t\ttype monitor\n"
        "\tInterface wlan0\n\t\ttype managed\n"
        "\tInterface p2p-wlan0-0\n\t\ttype P2P-GO\n";
    const auto iface = ParseP2pGoInterface(out);
    ASSERT_TRUE(iface.has_value());
    EXPECT_EQ(iface.value_or(""), "p2p-wlan0-0");
}

TEST(ParseP2pGoInterface, NulloptWhenNoGroupOwner) {
    EXPECT_FALSE(ParseP2pGoInterface("\tInterface wlan0\n\t\ttype managed\n").has_value());
    EXPECT_FALSE(ParseP2pGoInterface("").has_value());
}

TEST(ParseStationSignalDbm, ParsesNegativeSignal) {
    const std::string dump =
        "Station 26:f4:0a:68:c5:df (on wlP1p1s0)\n"
        "\tsignal:  \t-57 dBm\n"
        "\ttx bitrate:\t72.2 MBit/s\n";
    const auto rssi = ParseStationSignalDbm(dump);
    ASSERT_TRUE(rssi.has_value());
    EXPECT_EQ(rssi.value_or(0), -57);
}

TEST(ParseStationSignalDbm, NulloptWhenNoStationConnected) {
    EXPECT_FALSE(ParseStationSignalDbm("").has_value());
    EXPECT_FALSE(
        ParseStationSignalDbm("Station 26:f4:0a:68:c5:df (on wlP1p1s0)\n\tinactive time:\t4 ms\n")
            .has_value());
}

}  // namespace
