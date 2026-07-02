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

TEST(ParseP2pGoInterface, DoesNotFalseMatchP2pGoSubstring) {
    // Exact-match, not substr: a "P2P-GO" substring inside an SSID or a
    // "P2P-GO-client" type on a managed-only radio must not select an interface.
    const std::string out =
        "\tInterface wlan0\n\t\tssid DIRECT-type P2P-GO\n\t\ttype managed\n"
        "\tInterface wlan1\n\t\ttype P2P-GO-client\n";
    EXPECT_FALSE(ParseP2pGoInterface(out).has_value());
}

TEST(ParseP2pGoInterface, ToleratesCrlfLineEndings) {
    // getline leaves a trailing '\r' on CRLF input; the trim must still match.
    const auto iface = ParseP2pGoInterface("\tInterface wlP1p1s0\r\n\t\ttype P2P-GO\r\n");
    ASSERT_TRUE(iface.has_value());
    EXPECT_EQ(iface.value_or(""), "wlP1p1s0");
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

TEST(ParseStationSignalDbm, PicksSignalNotSignalAvgAndFirstStation) {
    // Real dumps carry both "signal:" and "signal avg:", and may list several
    // stations. Return the first station's instantaneous "signal:", never the avg.
    const std::string dump =
        "Station aa:bb:cc:dd:ee:ff (on wlP1p1s0)\n"
        "\tsignal:  \t-57 dBm\n"
        "\tsignal avg:\t-60 dBm\n"
        "Station 11:22:33:44:55:66 (on wlP1p1s0)\n"
        "\tsignal:  \t-70 dBm\n";
    EXPECT_EQ(ParseStationSignalDbm(dump).value_or(0), -57);
}

TEST(ParseStationSignalDbm, RejectsNonNegativeGarbage) {
    // A real RSSI is always negative; a non-negative parse is a corrupt line and
    // must read as unknown rather than a bogus positive "signal".
    EXPECT_FALSE(
        ParseStationSignalDbm("Station x (on wlP1p1s0)\n\tsignal:  \t42 dBm\n").has_value());
}

}  // namespace
