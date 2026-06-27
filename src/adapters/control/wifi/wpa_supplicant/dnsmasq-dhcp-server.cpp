#include "adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sst::adapters::control {

namespace {
constexpr const char* kDhcpRangeStart = "192.168.49.10";
constexpr const char* kDhcpRangeEnd = "192.168.49.50";
constexpr const char* kLeaseTime = "1h";
// Conventional shell "command not found" exit status, used when execvp fails.
constexpr int kExecFailedExitCode = 127;

// dnsmasq exits non-zero on a bind failure (port 67 held by an orphan, or dnsmasq
// missing). Sampling liveness only at t=0 missed a slightly-later bind failure and
// let the handler report DHCP "up" while it was dead. Watch a short window
// (~500ms) — surviving it is the bound/listening signal.
constexpr int kListenCheckAttempts = 10;
constexpr auto kListenCheckInterval = std::chrono::milliseconds(50);

// Best-effort: kill any orphan dnsmasq a prior firmware run left bound to this
// group interface. After a SIGKILL/OOM the in-memory pid_ is lost (resets to -1),
// so Start()'s pid_>0 teardown can't see the orphan; the new dnsmasq then fails to
// bind (port 67 still held) and DHCP silently dies — every reconnect after the
// crash shows the app "wifi failed". The firmware and the orphan share the same
// uid, so the kill is permitted. pkill matches the full command line, scoped by
// the interface token so unrelated dnsmasq instances are untouched.
void SweepStaleDnsmasq(const std::string& group_interface) {
    const std::string pattern = fmt::format("dnsmasq.*--interface={}", group_interface);
    const int pid = ::fork();
    if (pid < 0) {
        spdlog::warn("DnsmasqDhcpServer: orphan-sweep fork failed: {}", std::strerror(errno));
        return;
    }
    if (pid == 0) {
        // NOLINTNEXTLINE(readability-magic-numbers)
        std::array<const char*, 4> argv{"pkill", "-f", pattern.c_str(), nullptr};
        ::execvp("pkill", const_cast<char* const*>(argv.data()));
        ::_exit(kExecFailedExitCode);  // pkill missing — best-effort, ignored by parent
    }
    ::waitpid(pid, nullptr, 0);  // exit status irrelevant (1 = nothing matched)
}
}  // namespace

DnsmasqDhcpServer::~DnsmasqDhcpServer() { Stop(); }

auto DnsmasqDhcpServer::Start(const std::string& group_interface,
                              const std::string& go_ip) -> bool {
    // Idempotent: a dnsmasq from a prior StartWifiDirect can still be running when
    // the app re-requests a preview without a clean StopWifiDirect (abrupt BLE
    // drop skips session cleanup). Tear the old one down and start fresh instead
    // of failing with "already running" — that false return surfaced to the app
    // as "failed to start DHCP" and blocked every preview after the first.
    if (pid_ > 0) {
        spdlog::info("DnsmasqDhcpServer: restarting (prior pid={})", pid_);
        Stop();
    }
    if (group_interface.empty()) {
        spdlog::error("DnsmasqDhcpServer: empty group interface");
        return false;
    }

    // Clear any orphan dnsmasq from a prior crash before binding (pid_ teardown
    // above only catches a clean in-process restart). See SweepStaleDnsmasq.
    SweepStaleDnsmasq(group_interface);

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
        // --leasefile-ro: the service runs as non-root sst-cam and cannot write the
        // default lease file under root-only /var/lib/misc; P2P leases are ephemeral
        // so read-only leasing (no lease file) is correct, not a workaround.
        // Size is the count of the adjacent initializer elements (9 argv tokens +
        // the nullptr terminator); a named constant adds no clarity.
        // NOLINTNEXTLINE(readability-magic-numbers)
        std::array<const char*, 10> argv{
            "dnsmasq",      "--keep-in-foreground",  "--bind-interfaces",
            listen.c_str(), "--except-interface=lo", range.c_str(),
            router.c_str(), "--no-resolv",           "--leasefile-ro",
            nullptr};
        ::execvp("dnsmasq", const_cast<char* const*>(argv.data()));
        // Only reached if exec fails.
        ::_exit(kExecFailedExitCode);
    }

    // Parent: poll a short bind window. If dnsmasq exits during it (bind failure
    // or dnsmasq missing — the in-container case), report failure instead of
    // returning OK on a dead DHCP. Surviving the window means it bound and is
    // listening, closing the race where the app associated before DHCP was ready.
    for (int attempt = 0; attempt < kListenCheckAttempts; ++attempt) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            spdlog::error("DnsmasqDhcpServer: dnsmasq exited during bind window (status={})",
                          status);
            pid_ = -1;
            return false;
        }
        std::this_thread::sleep_for(kListenCheckInterval);
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
