#include "adapters/control/ble/bluez/device1-signal.hpp"

#include <map>
#include <string>

namespace sst::adapters::control {

auto IsDeviceNodeOf(
    const std::string& adapter_path,  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok:
    const std::string& object_path)  // an adapter path never parses as a device node, so a swap can
                                     // only ever return false
    -> bool {
    const std::string prefix = adapter_path + "/dev_";
    return object_path.size() > prefix.size() && object_path.compare(0, prefix.size(), prefix) == 0;
}

auto IsDeviceDisconnectSignal(const std::string& interface,
                              const std::map<std::string, sdbus::Variant>& changed) -> bool {
    if (interface != "org.bluez.Device1") {
        return false;
    }
    const auto entry = changed.find("Connected");
    if (entry == changed.end()) {
        return false;
    }
    try {
        return !entry->second.get<bool>();
    } catch (const sdbus::Error&) {
        return false;  // wrong-typed Connected — not a link-state change
    }
}

}  // namespace sst::adapters::control
