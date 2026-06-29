#include "adapters/control/network/nmcli-uplink-configurator.hpp"

#include <spdlog/spdlog.h>

#include <sstream>
#include <string>
#include <vector>

#include "adapters/control/network/nmcli-command.hpp"
#include "adapters/control/network/subprocess.hpp"

namespace sst::adapters::control {

namespace {
// Run a state-changing `nmcli` invocation, bounded by the apply deadline (a
// hung `con up` mid-NM-restart must not stall the BLE dispatcher thread).
auto RunNmcli(const std::vector<std::string>& argv) -> bool {
    return RunBounded(argv, kApplyTimeout);
}

// Capture stdout of a read-only `nmcli` query, bounded by the short query
// deadline. Used only for discovery (the connection name + current address),
// never to mutate state. Returns empty on timeout/failure.
auto Capture(const std::vector<std::string>& argv) -> std::string {
    return CaptureBounded(argv, kQueryTimeout).output;
}
}  // namespace

auto NmcliUplinkConfigurator::DetectEthernetConnection() -> std::string {
    // Lines: "DEVICE:TYPE:STATE:CONNECTION", e.g. "enP8p1s0:ethernet:connected:Wired connection 1".
    const std::string out =
        Capture({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"});
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        // Split into at most 4 fields on ':' (CONNECTION itself may contain spaces
        // but not ':'); the connection name is everything after the 3rd colon.
        const auto col1 = line.find(':');
        if (col1 == std::string::npos) {
            continue;
        }
        const auto col2 = line.find(':', col1 + 1);
        const auto col3 = (col2 == std::string::npos) ? col2 : line.find(':', col2 + 1);
        if (col2 == std::string::npos || col3 == std::string::npos) {
            continue;
        }
        const std::string type = line.substr(col1 + 1, col2 - col1 - 1);
        const std::string state = line.substr(col2 + 1, col3 - col2 - 1);
        const std::string connection = line.substr(col3 + 1);
        // First managed ethernet device with a real connection (skip unmanaged
        // usb0/usb1 gadget ifaces, which have no connection).
        if (type == "ethernet" && state != "unmanaged" && !connection.empty() &&
            connection != "--") {
            return connection;
        }
    }
    return {};
}

auto NmcliUplinkConfigurator::CurrentAddress(const std::string& connection) -> std::string {
    if (connection.empty()) {
        return {};
    }
    // "IP4.ADDRESS[1]:10.10.1.30/24" → take the value after the colon.
    const std::string out =
        Capture({"nmcli", "-t", "-g", "IP4.ADDRESS", "con", "show", connection});
    std::string address = out;
    const auto newline = address.find('\n');
    if (newline != std::string::npos) {
        address.erase(newline);
    }
    return address;
}

auto NmcliUplinkConfigurator::ApplyEthernet(const sst::config::EthernetUplink& cfg)
    -> sst::network::UplinkResult {
    const std::string connection = DetectEthernetConnection();
    if (connection.empty()) {
        return {.ok = false, .detail = "no managed ethernet connection found"};
    }

    const bool dhcp = cfg.dhcp.value_or(true);
    const auto mod_argv = dhcp ? BuildEthDhcpArgv(connection) : BuildEthStaticArgv(connection, cfg);
    if (mod_argv.empty()) {
        return {.ok = false, .detail = "invalid static config (need address CIDR)"};
    }
    if (!RunNmcli(mod_argv)) {
        return {.ok = false, .detail = "nmcli con mod failed"};
    }
    if (!RunNmcli(BuildConnectionUpArgv(connection))) {
        return {.ok = false, .detail = "nmcli con up failed"};
    }

    const std::string address = CurrentAddress(connection);
    spdlog::info("NmcliUplinkConfigurator: ethernet '{}' up ({}, {})", connection,
                 dhcp ? "dhcp" : "static", address);
    return {.ok = true, .detail = address};
}

auto NmcliUplinkConfigurator::ApplyWifiSta(const sst::config::WifiStaUplink& /*cfg*/)
    -> sst::network::UplinkResult {
    // GATED: this single-radio Jetson runs the WiFi-Direct GO (live preview) on
    // wlP1p1s0. A concurrent STA join on the same radio is unvalidated (regdomain
    // + single-channel concurrency); attempting it risks tearing down the GO that
    // carries preview. Until validated on-device, report unavailable so v1 uses
    // ethernet only, without disturbing the preview plane.
    return {.ok = false,
            .detail = "wifi uplink unavailable: single radio is dedicated to the WiFi-Direct GO"};
}

auto NmcliUplinkConfigurator::ProbeEthernet() -> sst::network::UplinkResult {
    // Live state, independent of the firmware uplink config: the camera's
    // ethernet is NM-managed, so it may be up with an address even when the
    // uplink config has it disabled. up == a managed connection with an address.
    const std::string connection = DetectEthernetConnection();
    if (connection.empty()) {
        return {.ok = false, .detail = "no ethernet connection"};
    }
    const std::string address = CurrentAddress(connection);
    return {.ok = !address.empty(), .detail = address.empty() ? "no address" : address};
}

auto NmcliUplinkConfigurator::ProbeWifiSta() -> sst::network::UplinkResult {
    // The wifi-STA uplink is gated (single radio = the WiFi-Direct GO), so it is
    // never up; report the same reason the apply path does.
    return {.ok = false,
            .detail = "wifi uplink unavailable: single radio is dedicated to the WiFi-Direct GO"};
}

}  // namespace sst::adapters::control
