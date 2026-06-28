#include "adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp"
#include "domain/network/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::adapters::control {

namespace {

constexpr std::size_t kRecvBufSize = 4096;
constexpr auto kRecvTimeout = std::chrono::seconds{2};
constexpr const char* kGoIpAddress = "192.168.49.1";
constexpr const char* kGoRole = "GO";
// Budget for skipping unsolicited events while scanning for a command reply or
// a target event. After ATTACH + P2P_GROUP_REMOVE the socket can carry a burst
// of events, so this must be comfortably larger than that burst or a real reply
// (P2P_GROUP_ADD's OK) gets dropped as "<no reply>".
constexpr int kMaxEventReads = 48;

auto StartsWith(const std::string& text, std::string_view prefix) -> bool {
    return text.size() >= prefix.size() &&
           std::memcmp(text.data(), prefix.data(), prefix.size()) == 0;
}

// A WiFi-ish interface name (predictable "wlP1p1s0", classic "wlan0", or a P2P
// virtual iface).
auto IsWifiName(const std::string& name) -> bool {
    return StartsWith(name, "wl") || StartsWith(name, "p2p");
}

// The wpa_supplicant-managed interface == the control socket present in ctrl_dir.
// This is the most reliable signal: it is exactly the socket we then connect to.
auto DetectFromCtrlDir(const std::string& ctrl_dir) -> std::optional<std::string> {
    std::error_code errc;
    if (!std::filesystem::is_directory(ctrl_dir, errc)) {
        return std::nullopt;
    }
    std::optional<std::string> first;
    for (const auto& entry : std::filesystem::directory_iterator(ctrl_dir, errc)) {
        if (errc) {
            break;
        }
        auto name = entry.path().filename().string();
        if (!entry.is_socket(errc)) {
            continue;
        }
        if (IsWifiName(name)) {
            return name;  // prefer a wl*/p2p* socket
        }
        if (!first) {
            first = name;
        }
    }
    return first;
}

// Fallback: a wireless netdev under sysfs (one exposing a phy80211 link).
auto DetectFromSysfs(const std::string& sysfs_dir) -> std::optional<std::string> {
    std::error_code errc;
    if (!std::filesystem::is_directory(sysfs_dir, errc)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator(sysfs_dir, errc)) {
        if (errc) {
            break;
        }
        if (std::filesystem::exists(entry.path() / "phy80211", errc)) {
            return entry.path().filename().string();
        }
    }
    return std::nullopt;
}

}  // namespace

auto IsWpaUnsolicitedEvent(const std::string& msg) -> bool {
    return !msg.empty() && msg.front() == '<';
}

// Public signature with distinct roles consumed positionally by external
// callers; reordering would break the contract — hence the suppression.
auto ResolveWifiInterface(
    const std::string& requested,  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok:
                                   // fixed (requested, ctrl_dir, sysfs_dir) public signature
    const std::string& ctrl_dir, const std::string& sysfs_dir) -> std::string {
    if (!requested.empty() && requested != "auto") {
        return requested;
    }
    if (auto iface = DetectFromCtrlDir(ctrl_dir)) {
        spdlog::info("WpaWifiManager: detected wifi interface '{}' from {}", *iface, ctrl_dir);
        return *iface;
    }
    if (auto iface = DetectFromSysfs(sysfs_dir)) {
        spdlog::info("WpaWifiManager: detected wifi interface '{}' from {}", *iface, sysfs_dir);
        return *iface;
    }
    spdlog::warn(
        "WpaWifiManager: no wifi interface detected under {} or {}; falling back to 'wlan0'",
        ctrl_dir, sysfs_dir);
    return "wlan0";
}

WpaWifiManager::WpaWifiManager(std::string iface, std::string ctrl_dir)
    : iface_(std::move(iface)), ctrl_dir_(std::move(ctrl_dir)) {}

WpaWifiManager::~WpaWifiManager() { CloseCtrlSocket(); }

auto WpaWifiManager::OpenCtrlSocket() -> bool {
    if (sock_ >= 0) {
        return true;
    }

    // Resolve "auto" to the real interface lazily (here, not in the ctor): the
    // control socket and group permissions are in place by connect time.
    iface_ = ResolveWifiInterface(iface_, ctrl_dir_);

    sock_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        spdlog::error("WpaWifiManager: socket() failed: {}", std::strerror(errno));
        return false;
    }

    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    local_path_ = fmt::format("/tmp/wpa_ctrl_{}-{}", ::getpid(), std::rand());
    std::strncpy(local.sun_path, local_path_.c_str(), sizeof(local.sun_path) - 1);
    ::unlink(local.sun_path);
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        spdlog::error("WpaWifiManager: bind({}) failed: {}", local_path_, std::strerror(errno));
        CloseCtrlSocket();
        return false;
    }

    sockaddr_un remote{};
    remote.sun_family = AF_UNIX;
    const auto remote_path = fmt::format("{}/{}", ctrl_dir_, iface_);
    std::strncpy(remote.sun_path, remote_path.c_str(), sizeof(remote.sun_path) - 1);
    if (::connect(sock_, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
        spdlog::error("WpaWifiManager: connect({}) failed: {}", remote_path, std::strerror(errno));
        CloseCtrlSocket();
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(kRecvTimeout).count();
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    spdlog::info("WpaWifiManager: ctrl_iface connected via {}", remote_path);
    return true;
}

auto WpaWifiManager::CloseCtrlSocket() -> void {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
    if (!local_path_.empty()) {
        ::unlink(local_path_.c_str());
        local_path_.clear();
    }
}

auto WpaWifiManager::SendCommand(std::string_view cmd) -> std::optional<std::string> {
    if (sock_ < 0 && !OpenCtrlSocket()) {
        return std::nullopt;
    }

    if (::send(sock_, cmd.data(), cmd.size(), 0) < 0) {
        spdlog::error("WpaWifiManager: send(\"{}\") failed: {}", cmd, std::strerror(errno));
        return std::nullopt;
    }

    // Skip unsolicited events ("<priority>..." datagrams that ATTACH turns on)
    // and return the first real reply. Bounded so a flood can't spin forever.
    for (int i = 0; i < kMaxEventReads; ++i) {
        std::array<char, kRecvBufSize> buf{};
        const auto bytes = ::recv(sock_, buf.data(), buf.size() - 1, 0);
        if (bytes < 0) {
            spdlog::error("WpaWifiManager: recv after \"{}\" failed: {}", cmd,
                          std::strerror(errno));
            return std::nullopt;
        }
        if (bytes == 0) {
            // A zero-length datagram is not a reply. ReadUntil treats bytes<=0 as
            // terminal; here we skip the empty datagram and keep scanning for the
            // real reply within the bounded loop rather than returning "" as if it
            // were the command's response (which made StartsWith("OK") spuriously
            // fail, e.g. P2P_GROUP_ADD).
            continue;
        }
        std::string msg(buf.data(), static_cast<std::size_t>(bytes));
        if (IsWpaUnsolicitedEvent(msg)) {
            continue;
        }
        return msg;
    }
    return std::nullopt;
}

auto WpaWifiManager::ReadUntil(std::string_view marker) const -> std::optional<std::string> {
    for (int i = 0; i < kMaxEventReads; ++i) {
        std::array<char, kRecvBufSize> buf{};
        const auto bytes = ::recv(sock_, buf.data(), buf.size() - 1, 0);
        if (bytes <= 0) {
            return std::nullopt;  // timeout / closed
        }
        std::string msg(buf.data(), static_cast<std::size_t>(bytes));
        if (msg.find(marker) != std::string::npos) {
            return msg;
        }
    }
    return std::nullopt;
}

auto WpaWifiManager::DrainPendingEvents() const -> void {
    if (sock_ < 0) {
        return;
    }
    // Non-blocking: discard any queued unsolicited events so the next command's
    // reply is found within SendCommand's event budget. Bounded so a continuous
    // event stream can't spin here forever.
    for (int i = 0; i < kMaxEventReads; ++i) {
        std::array<char, kRecvBufSize> buf{};
        const auto bytes = ::recv(sock_, buf.data(), buf.size() - 1, MSG_DONTWAIT);
        if (bytes <= 0) {
            break;  // EAGAIN/EWOULDBLOCK (nothing pending) or closed
        }
    }
}

auto WpaWifiManager::StartP2pGroupOwner() -> std::optional<sst::network::WifiDirectGroup> {
    std::lock_guard lock(mtx_);
    if (!OpenCtrlSocket()) {
        return std::nullopt;
    }

    // Idempotency: a P2P group left over from a prior session — or from a
    // firmware restart that left wpa_supplicant's GO up on the radio — makes a
    // fresh P2P_GROUP_ADD never emit P2P-GROUP-STARTED, so every preview after
    // the first failed with "failed to form WiFi Direct group owner". Tear down
    // any existing group first. Best-effort: OK (removed) and FAIL (none present)
    // are both acceptable. Done BEFORE ATTACH so this reply is not interleaved
    // with the unsolicited P2P-GROUP-REMOVED event it triggers.
    SendCommand("P2P_GROUP_REMOVE *");

    // Subscribe to unsolicited events so we capture P2P-GROUP-STARTED.
    SendCommand("ATTACH");

    // Form a real autonomous (non-persistent) group owner. wpa_supplicant
    // generates the SSID + passphrase. The FIRST P2P_GROUP_ADD right after a
    // BLE connect frequently comes back "<no reply>": its OK is lost among the
    // unsolicited events ATTACH just turned on (and the P2P_GROUP_REMOVE flush),
    // so SendCommand exhausts its event-skip budget before the reply — the
    // "wifi failed on first connect, works on reconnect" symptom. Retry the
    // whole add+await sequence, cleaning any partial group between tries. The
    // radio (NO-CARRIER until the GO comes up) needs ~1-2s to form a group, so
    // the retry delay is generous rather than spinning.
    constexpr int kGroupAddAttempts = 5;
    constexpr auto kGroupAddRetryDelay = std::chrono::seconds(1);
    std::optional<ParsedGroup> parsed;
    for (int attempt = 1; attempt <= kGroupAddAttempts; ++attempt) {
        // Flush any backlog of unsolicited events first so this attempt's OK
        // reply isn't skipped past by SendCommand's event budget.
        DrainPendingEvents();
        auto reply = SendCommand("P2P_GROUP_ADD");
        if (reply && StartsWith(*reply, "OK")) {
            if (auto event = ReadUntil("P2P-GROUP-STARTED")) {
                parsed = ParseGroupStarted(*event);
                if (parsed) {
                    break;
                }
                spdlog::warn("WpaWifiManager: could not parse P2P-GROUP-STARTED: {}", *event);
            } else {
                spdlog::warn("WpaWifiManager: no P2P-GROUP-STARTED (attempt {}/{})", attempt,
                             kGroupAddAttempts);
            }
        } else {
            spdlog::warn("WpaWifiManager: P2P_GROUP_ADD attempt {}/{} failed: {}", attempt,
                         kGroupAddAttempts, reply ? *reply : std::string{"<no reply>"});
        }
        if (attempt < kGroupAddAttempts) {
            SendCommand("P2P_GROUP_REMOVE *");  // drop any half-formed group before retrying
            std::this_thread::sleep_for(kGroupAddRetryDelay);
        }
    }
    if (!parsed) {
        spdlog::error("WpaWifiManager: P2P_GROUP_ADD failed after {} attempts", kGroupAddAttempts);
        return std::nullopt;
    }

    group_interface_ = parsed->interface;
    state_ = {.mode = sst::control::WifiMode::kP2pGroupOwner,
              .connected = true,
              .ssid = parsed->ssid,
              .ip_address = kGoIpAddress};

    sst::network::WifiDirectGroup group;
    group.ssid = parsed->ssid;
    group.psk = parsed->passphrase;
    group.group_interface = parsed->interface;
    group.group_owner_ip = kGoIpAddress;
    group.role = kGoRole;
    spdlog::info("WpaWifiManager::StartP2pGroupOwner formed group {}", group);
    return group;
}

auto WpaWifiManager::Stop() -> void {
    std::lock_guard lock(mtx_);
    if (sock_ >= 0) {
        if (!group_interface_.empty()) {
            SendCommand(fmt::format("P2P_GROUP_REMOVE {}", group_interface_));
        } else {
            SendCommand("P2P_GROUP_REMOVE *");
        }
    }
    group_interface_.clear();
    state_ = {};
    CloseCtrlSocket();
    spdlog::info("WpaWifiManager::Stop");
}

auto WpaWifiManager::State() const -> sst::control::WifiState {
    std::lock_guard lock(mtx_);
    return state_;
}

}  // namespace sst::adapters::control
