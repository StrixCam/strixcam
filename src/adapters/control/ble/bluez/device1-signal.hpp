#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <map>
#include <string>

namespace sst::adapters::control {

// Classification helpers for org.bluez.Device1 PropertiesChanged signals.
//
// A force-killed central never sends GATT StopNotify — the link just drops.
// BlueZ reports that drop as PropertiesChanged{Connected=false} on the device
// node (/org/bluez/hciN/dev_XX_...). The transport subscribes bus-wide (device
// paths are dynamic) and uses these pure helpers to decide whether a signal is
// a central link drop on OUR adapter; they are free functions so the decision
// is unit-testable without a bus.

// True iff `object_path` is a device node under `adapter_path`
// (e.g. /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF for adapter /org/bluez/hci0).
auto IsDeviceNodeOf(const std::string& adapter_path, const std::string& object_path) -> bool;

// True iff this PropertiesChanged payload announces a dropped link: interface
// org.bluez.Device1 with Connected=false among the changed properties. A
// missing or non-boolean Connected entry classifies as false (not a drop).
auto IsDeviceDisconnectSignal(const std::string& interface,
                              const std::map<std::string, sdbus::Variant>& changed) -> bool;

}  // namespace sst::adapters::control
