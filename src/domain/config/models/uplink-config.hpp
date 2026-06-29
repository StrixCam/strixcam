#pragma once

#include <optional>
#include <string>

namespace sst::config {

// Internet uplink configuration — the camera's path to the cloud (RTMP), kept
// SEPARATE from the WiFi-Direct GO that serves the phone its control + live
// preview. Persisted in /etc/sst/cam/config/uplink.json and pushed from the app
// over BLE (SetNetworkConfig). Two independent interfaces, each enable/disable
// with DHCP-or-static IP:
//   - ethernet (eth0): the guaranteed uplink when a cable is present.
//   - wifi (STA on the camera): GATED on single-radio GO+STA concurrency — the
//     camera joins a network (venue wifi, a phone hotspot) as a normal client.
// All fields optional so a partial JSON / partial BLE push leaves the rest at
// the firmware's last value (mirrors WifiDirectData).

// Static-vs-DHCP addressing shared by both uplink interfaces. `dhcp == true`
// (or unset) means DHCP and the static fields are ignored; `dhcp == false`
// means use `address`/`gateway`/`dns`. `address` is CIDR (e.g. 192.168.1.50/24).
struct EthernetUplink {
    std::optional<bool> enabled{std::nullopt};
    std::optional<bool> dhcp{std::nullopt};
    std::optional<std::string> address{std::nullopt};
    std::optional<std::string> gateway{std::nullopt};
    std::optional<std::string> dns{std::nullopt};
};

struct WifiStaUplink {
    std::optional<bool> enabled{std::nullopt};
    std::optional<std::string> ssid{std::nullopt};
    std::optional<std::string> passphrase{std::nullopt};
    std::optional<bool> dhcp{std::nullopt};
    std::optional<std::string> address{std::nullopt};
    std::optional<std::string> gateway{std::nullopt};
    std::optional<std::string> dns{std::nullopt};
};

struct UplinkData {
    EthernetUplink ethernet;
    WifiStaUplink wifi;
};

}  // namespace sst::config
