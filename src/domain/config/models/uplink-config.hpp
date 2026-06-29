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
// Fields are optional so a partial JSON on disk leaves the rest nullopt
// (mirrors WifiDirectData). NOTE on the BLE push: SetNetworkConfig is a FULL
// REPLACE of the uplink config, not a partial merge. proto3 scalars are
// always-present, so ProtoToUplink engages every optional from the incoming
// NetworkConfig — the app always sends the complete config and the firmware
// overwrites its current uplink wholesale. (A true field-mask / proto
// `optional` partial-push is a deferred cross-repo change.)

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
