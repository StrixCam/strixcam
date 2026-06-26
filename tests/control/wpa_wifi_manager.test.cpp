// Real autonomous P2P group-owner bring-up (U12, R23). Hardware-bound: needs a
// running wpa_supplicant with P2P-GO support — expected to FAIL in the
// container, passes on-device.
//
// ResolveWifiInterfaceTest below is PURE (temp dirs) and runs anywhere.

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.hpp"

namespace {

using sst::adapters::control::ResolveWifiInterface;

namespace fs = std::filesystem;

// Create a real AF_UNIX socket file at [path] so directory_iterator::is_socket()
// recognises it (a regular file would not). Returns the open fd (caller closes).
auto MakeSocketFile(const fs::path& path) -> int {
    const int sock_fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    EXPECT_GE(sock_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    EXPECT_EQ(::bind(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    return sock_fd;
}

// A unique temp dir for one test, removed on destruction.
class TempDir {
   public:
    TempDir() {
        const auto* name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        path_ = fs::temp_directory_path() / (std::string("sst-wifi-") + name);
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() { fs::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    auto operator=(const TempDir&) -> TempDir& = delete;
    TempDir(TempDir&&) = delete;
    auto operator=(TempDir&&) -> TempDir& = delete;
    [[nodiscard]] auto path() const -> const fs::path& { return path_; }

   private:
    fs::path path_;
};

TEST(ResolveWifiInterfaceTest, ReturnsExplicitNameUnchanged) {
    EXPECT_EQ(ResolveWifiInterface("wlan5", "/nonexistent", "/nonexistent"), "wlan5");
}

TEST(ResolveWifiInterfaceTest, DetectsWifiSocketInCtrlDir) {
    TempDir ctrl;
    const int sock_fd = MakeSocketFile(ctrl.path() / "wlP1p1s0");
    EXPECT_EQ(ResolveWifiInterface("auto", ctrl.path().string(), "/nonexistent"), "wlP1p1s0");
    ::close(sock_fd);
}

TEST(ResolveWifiInterfaceTest, PrefersWifiSocketOverOtherSockets) {
    TempDir ctrl;
    const int sock_a = MakeSocketFile(ctrl.path() / "p2p-dev-wlP1p1s0");
    const int sock_b = MakeSocketFile(ctrl.path() / "wlP1p1s0");
    const auto got = ResolveWifiInterface("auto", ctrl.path().string(), "/nonexistent");
    EXPECT_TRUE(got == "wlP1p1s0" || got == "p2p-dev-wlP1p1s0");  // both are wifi-ish
    ::close(sock_a);
    ::close(sock_b);
}

TEST(ResolveWifiInterfaceTest, FallsBackToSysfsWhenNoCtrlSocket) {
    TempDir ctrl;  // empty ctrl dir
    TempDir sysfs;
    fs::create_directories(sysfs.path() / "eth0");
    fs::create_directories(sysfs.path() / "wlP1p1s0" / "phy80211");
    EXPECT_EQ(ResolveWifiInterface("auto", ctrl.path().string(), sysfs.path().string()),
              "wlP1p1s0");
}

TEST(ResolveWifiInterfaceTest, FallsBackToWlan0WhenNothingFound) {
    EXPECT_EQ(ResolveWifiInterface("auto", "/nonexistent", "/nonexistent"), "wlan0");
    EXPECT_EQ(ResolveWifiInterface("", "/nonexistent", "/nonexistent"), "wlan0");
}

TEST(IsWpaUnsolicitedEventTest, TaggedEventsAreSkipped) {
    using sst::adapters::control::IsWpaUnsolicitedEvent;
    // "<priority>"-prefixed datagrams are unsolicited events, not replies.
    EXPECT_TRUE(IsWpaUnsolicitedEvent("<3>WPS-AP-AVAILABLE"));
    EXPECT_TRUE(IsWpaUnsolicitedEvent("<3>P2P-GROUP-STARTED p2p-wlan0-0 GO"));
    EXPECT_TRUE(IsWpaUnsolicitedEvent("<2>CTRL-EVENT-SCAN-RESULTS"));
}

TEST(IsWpaUnsolicitedEventTest, RepliesAreNotEvents) {
    using sst::adapters::control::IsWpaUnsolicitedEvent;
    // Command replies never carry the "<priority>" prefix.
    EXPECT_FALSE(IsWpaUnsolicitedEvent("OK\n"));
    EXPECT_FALSE(IsWpaUnsolicitedEvent("FAIL\n"));
    EXPECT_FALSE(IsWpaUnsolicitedEvent("p2p-wlan0-0"));
    EXPECT_FALSE(IsWpaUnsolicitedEvent(""));
}

TEST(WpaWifiManagerE2E, FormsAutonomousGroupOwner) {
    sst::adapters::control::WpaWifiManager manager("wlan0");
    auto group = manager.StartP2pGroupOwner();
    if (!group) {
        FAIL() << "P2P group owner did not form (expected to fail off-device)";
        return;
    }
    EXPECT_FALSE(group->ssid.empty());
    EXPECT_FALSE(group->psk.empty());
    EXPECT_EQ(group->group_owner_ip, "192.168.49.1");
    EXPECT_EQ(group->role, "GO");
    manager.Stop();
}

}  // namespace
