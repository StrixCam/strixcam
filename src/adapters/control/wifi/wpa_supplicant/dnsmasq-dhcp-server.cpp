#include "adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

namespace sst::adapters::control {

namespace {
constexpr const char* kDhcpRangeStart = "192.168.49.10";
constexpr const char* kDhcpRangeEnd = "192.168.49.50";
constexpr const char* kLeaseTime = "1h";
// Conventional shell "command not found" exit status, used when execvp fails.
constexpr int kExecFailedExitCode = 127;
}  // namespace

DnsmasqDhcpServer::~DnsmasqDhcpServer() { Stop(); }

auto DnsmasqDhcpServer::Start(const std::string& group_interface, const std::string& go_ip)
    -> bool {
    if (pid_ > 0) {
        spdlog::warn("DnsmasqDhcpServer: already running (pid={})", pid_);
        return false;
    }
    if (group_interface.empty()) {
        spdlog::error("DnsmasqDhcpServer: empty group interface");
        return false;
    }

    // Assigning the static GO IP to the interface and source-based policy routing
    // for cellular coexistence are deploy-time provisioning steps (KTD4); here we
    // launch dnsmasq bound to the group interface only.
    const std::string dhcp_range =
        fmt::format("{},{},{}", kDhcpRangeStart, kDhcpRangeEnd, kLeaseTime);
    const std::string listen = fmt::format("--interface={}", group_interface);
    const std::string range = fmt::format("--dhcp-range={}", dhcp_range);
    const std::string router = fmt::format("--dhcp-option=3,{}", go_ip);

    const int pid = ::fork();
    if (pid < 0) {
        spdlog::error("DnsmasqDhcpServer: fork failed: {}", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child: exec dnsmasq in the foreground, bound to the group interface.
        // Size is the count of the adjacent initializer elements (8 argv tokens +
        // the nullptr terminator); a named constant adds no clarity.
        // NOLINTNEXTLINE(readability-magic-numbers)
        std::array<const char*, 9> argv{
            "dnsmasq",      "--keep-in-foreground",  "--bind-interfaces",
            listen.c_str(), "--except-interface=lo", range.c_str(),
            router.c_str(), "--no-resolv",           nullptr};
        ::execvp("dnsmasq", const_cast<char* const*>(argv.data()));
        // Only reached if exec fails.
        ::_exit(kExecFailedExitCode);
    }

    // Parent: brief check that the child didn't immediately die (e.g. dnsmasq
    // missing — the in-container case).
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
        spdlog::error("DnsmasqDhcpServer: dnsmasq exited immediately (status={})", status);
        pid_ = -1;
        return false;
    }
    pid_ = pid;
    spdlog::info("DnsmasqDhcpServer: serving DHCP on {} (go_ip={}, pid={})", group_interface, go_ip,
                 pid_);
    return true;
}

auto DnsmasqDhcpServer::Stop() -> void {
    if (pid_ <= 0) {
        return;
    }
    ::kill(pid_, SIGTERM);
    ::waitpid(pid_, nullptr, 0);
    spdlog::info("DnsmasqDhcpServer: stopped (pid={})", pid_);
    pid_ = -1;
}

}  // namespace sst::adapters::control
