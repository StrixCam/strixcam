// Abrupt central loss detection: classification of org.bluez.Device1
// PropertiesChanged signals (the second central-gone path — a force-killed
// app sends no GATT StopNotify; the link just drops and BlueZ flips
// Device1.Connected to false).
//
// Pure — no bus. sdbus::Variant values are constructed standalone. The live
// end-to-end (BlueZ emitting Connected=false on a real link drop feeding
// HandleCentralGone → re-advertise) is hardware-bound and exercised on-device.

#include <gtest/gtest.h>
#include <sdbus-c++/sdbus-c++.h>

#include <map>
#include <string>

#include "adapters/control/ble/bluez/device1-signal.hpp"

namespace {

using sst::adapters::control::IsDeviceDisconnectSignal;
using sst::adapters::control::IsDeviceNodeOf;

constexpr const char* kDevice1 = "org.bluez.Device1";

TEST(Device1SignalTest, ConnectedFalseOnDevice1IsDisconnect) {
    const std::map<std::string, sdbus::Variant> changed{{"Connected", sdbus::Variant{false}}};
    EXPECT_TRUE(IsDeviceDisconnectSignal(kDevice1, changed));
}

TEST(Device1SignalTest, ConnectedTrueIsNotDisconnect) {
    const std::map<std::string, sdbus::Variant> changed{{"Connected", sdbus::Variant{true}}};
    EXPECT_FALSE(IsDeviceDisconnectSignal(kDevice1, changed));
}

TEST(Device1SignalTest, OtherInterfaceIsIgnored) {
    const std::map<std::string, sdbus::Variant> changed{{"Connected", sdbus::Variant{false}}};
    EXPECT_FALSE(IsDeviceDisconnectSignal("org.bluez.MediaControl1", changed));
}

TEST(Device1SignalTest, UnrelatedPropertyChangesAreIgnored) {
    // RSSI / ServicesResolved churn on a live connection must not classify as
    // a link drop.
    const std::map<std::string, sdbus::Variant> changed{{"RSSI", sdbus::Variant{std::int16_t{-42}}},
                                                        {"ServicesResolved", sdbus::Variant{true}}};
    EXPECT_FALSE(IsDeviceDisconnectSignal(kDevice1, changed));
}

TEST(Device1SignalTest, WrongTypedConnectedIsIgnoredNotThrown) {
    const std::map<std::string, sdbus::Variant> changed{
        {"Connected", sdbus::Variant{std::string{"false"}}}};
    EXPECT_FALSE(IsDeviceDisconnectSignal(kDevice1, changed));
}

TEST(Device1SignalTest, DeviceNodePathsMatchOnlyOwnAdapter) {
    const std::string adapter = "/org/bluez/hci0";
    EXPECT_TRUE(IsDeviceNodeOf(adapter, "/org/bluez/hci0/dev_4E_2E_65_91_7C_0D"));
    // Another adapter's device is not ours.
    EXPECT_FALSE(IsDeviceNodeOf(adapter, "/org/bluez/hci1/dev_4E_2E_65_91_7C_0D"));
    // The adapter node itself (or a bare prefix) is not a device node.
    EXPECT_FALSE(IsDeviceNodeOf(adapter, "/org/bluez/hci0"));
    EXPECT_FALSE(IsDeviceNodeOf(adapter, "/org/bluez/hci0/dev_"));
    EXPECT_FALSE(IsDeviceNodeOf(adapter, ""));
}

}  // namespace
