#include "adapters/control/network/nmcli-uplink-configurator.hpp"

#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "adapters/control/network/nmcli-command.hpp"

namespace sst::adapters::control {

namespace {
constexpr int kExecFailedExitCode = 127;
constexpr std::size_t kReadBufferSize = 256;

// Run `nmcli` with [argv] to completion; true iff it exits 0. Mirrors the
// IpNetworkConfigurator fork/execvp style (nmcli con mod/up are short-lived).
auto RunNmcli(const std::vector<std::string>& argv) -> bool {
    if (argv.empty()) {
        return false;
    }
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        c_argv.push_back(
            const_cast<char*>(arg.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    c_argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("NmcliUplinkConfigurator: fork failed: {}", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        ::execvp("nmcli", c_argv.data());
        ::_exit(kExecFailedExitCode);  // only reached if exec fails
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Capture stdout of a read-only `nmcli` query. Used only for discovery (the
// connection name + current address), never to mutate state.
auto Capture(const std::string& command) -> std::string {
    std::array<char, kReadBufferSize> buffer{};
    std::string out;
    // NOLINTNEXTLINE(cert-env33-c) — fixed nmcli query strings, no user input in the command.
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return out;
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
    ::pclose(pipe);
    return out;
}
}  // namespace

auto NmcliUplinkConfigurator::DetectEthernetConnection() -> std::string {
    // Lines: "DEVICE:TYPE:STATE:CONNECTION", e.g. "enP8p1s0:ethernet:connected:Wired connection 1".
    const std::string out = Capture("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status");
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
    const std::string out = Capture("nmcli -t -g IP4.ADDRESS con show \"" + connection + "\"");
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

}  // namespace sst::adapters::control
