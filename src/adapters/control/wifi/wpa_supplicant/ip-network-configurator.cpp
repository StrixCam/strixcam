#include "adapters/control/wifi/wpa_supplicant/ip-network-configurator.hpp"

#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "adapters/control/wifi/wpa_supplicant/ip-command.hpp"

namespace sst::adapters::control {

namespace {
// Conventional shell "command not found" exit status, used when execvp fails.
constexpr int kExecFailedExitCode = 127;

// Run `ip` with [argv] to completion; true iff it exits 0. Mirrors the
// DnsmasqDhcpServer fork/execvp style but synchronous (ip is short-lived).
auto RunIp(const std::vector<std::string>& argv) -> bool {
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
        spdlog::error("IpNetworkConfigurator: fork failed: {}", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        ::execvp("ip", c_argv.data());
        ::_exit(kExecFailedExitCode);  // only reached if exec fails
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}  // namespace

auto IpNetworkConfigurator::AssignGroupOwnerAddress(const std::string& iface,
                                                    const std::string& cidr) -> bool {
    const auto add_argv = BuildAddrAddArgv(cidr, iface);
    const auto up_argv = BuildLinkUpArgv(iface);
    if (add_argv.empty() || up_argv.empty()) {
        spdlog::error("IpNetworkConfigurator: invalid iface '{}' / cidr '{}'", iface, cidr);
        return false;
    }
    if (!RunIp(add_argv)) {
        spdlog::error("IpNetworkConfigurator: `ip addr add {} dev {}` failed", cidr, iface);
        return false;
    }
    // The wpa-created group iface is usually already up; bringing it up is
    // best-effort and must not fail the assignment.
    if (!RunIp(up_argv)) {
        spdlog::warn("IpNetworkConfigurator: `ip link set {} up` returned non-zero", iface);
    }
    spdlog::info("IpNetworkConfigurator: assigned {} to {}", cidr, iface);
    return true;
}

auto IpNetworkConfigurator::Clear(const std::string& iface) -> void {
    if (iface.empty()) {
        return;
    }
    // Best-effort: the group iface usually disappears at P2P_GROUP_REMOVE; flushing
    // guards against a reused iface name carrying a stale address.
    RunIp({"ip", "addr", "flush", "dev", iface});
}

}  // namespace sst::adapters::control
