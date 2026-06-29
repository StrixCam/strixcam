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
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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

// One row of `LIST_NETWORKS` output.
struct NetworkRow {
    std::string id;
    std::string ssid;
    std::string flags;
};

// Parse a `LIST_NETWORKS` reply (`id<TAB>ssid<TAB>bssid<TAB>flags` rows after a
// header) into rows. Lines without all three tabs (the header, blanks) are
// skipped.
auto ParseNetworks(const std::string& list_reply) -> std::vector<NetworkRow> {
    std::vector<NetworkRow> rows;
    std::istringstream stream(list_reply);
    std::string line;
    while (std::getline(stream, line)) {
        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            continue;
        }
        const auto second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos) {
            continue;
        }
        const auto third_tab = line.find('\t', second_tab + 1);
        if (third_tab == std::string::npos) {
            continue;
        }
        rows.push_back({.id = line.substr(0, first_tab),
                        .ssid = line.substr(first_tab + 1, second_tab - first_tab - 1),
                        .flags = line.substr(third_tab + 1)});
    }
    return rows;
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

WpaWifiManager::WpaWifiManager(std::string iface, std::string ctrl_dir, std::string ssid_postfix)
    : iface_(std::move(iface)),
      ctrl_dir_(std::move(ctrl_dir)),
      ssid_postfix_(std::move(ssid_postfix)) {}

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

auto WpaWifiManager::DisableStaNetworks() -> void {
    // The device's WiFi radio is dedicated to WiFi-Direct (the GO the app joins);
    // it must NEVER act as a station on a home AP. A single radio can't be both a
    // STA on one channel and a GO on another — a stray STA association (e.g.
    // wpa_supplicant auto-joining a saved network) steals the channel and starves
    // the GO's DHCP + video throughput. Internet, when needed, is via WiFi-Direct
    // or ethernet (dev only). So drop any current association and disable every
    // non-P2P (station) network, leaving the radio free for the GO.
    SendCommand("DISCONNECT");
    auto reply = SendCommand("LIST_NETWORKS");
    if (!reply) {
        return;
    }
    for (const auto& net : ParseNetworks(*reply)) {
        // Station networks only — never a P2P group (active or persistent). Every
        // P2P SSID is `DIRECT-…`; a home AP is not, so the prefix is the safe
        // discriminator (the [CURRENT] flag alone can't tell them apart).
        if (!StartsWith(net.ssid, "DIRECT-")) {
            SendCommand(fmt::format("DISABLE_NETWORK {}", net.id));
        }
    }
}

auto WpaWifiManager::FindNamedPersistentGroupId() -> std::optional<std::string> {
    if (ssid_postfix_.empty()) {
        return std::nullopt;
    }
    auto reply = SendCommand("LIST_NETWORKS");
    if (!reply) {
        return std::nullopt;
    }
    // Our persistent group is the STORED definition (flagged [P2P-PERSISTENT],
    // not the transient [CURRENT] active row — reusing the active id fails) whose
    // SSID carries the device postfix (`DIRECT-XY-sst-cam-NNNN`). Only this
    // firmware names it that way, so the match is unambiguous.
    for (const auto& net : ParseNetworks(*reply)) {
        if (net.flags.find("P2P-PERSISTENT") != std::string::npos &&
            net.ssid.find(ssid_postfix_) != std::string::npos) {
            return net.id;
        }
    }
    return std::nullopt;
}

auto WpaWifiManager::FormGroup(std::string_view add_cmd)
    -> std::optional<sst::network::WifiDirectGroup> {
    // The FIRST P2P_GROUP_ADD right after a BLE connect frequently comes back
    // "<no reply>": its OK is lost among the unsolicited events ATTACH just
    // turned on (and the P2P_GROUP_REMOVE flush), so SendCommand exhausts its
    // event-skip budget before the reply — the "wifi failed on first connect,
    // works on reconnect" symptom. Retry the whole add+await sequence, cleaning
    // any partial group between tries. The radio (NO-CARRIER until the GO comes
    // up) needs ~1-2s to form a group, so the retry delay is generous.
    constexpr int kGroupAddAttempts = 5;
    constexpr auto kGroupAddRetryDelay = std::chrono::seconds(1);
    for (int attempt = 1; attempt <= kGroupAddAttempts; ++attempt) {
        // Flush any backlog of unsolicited events first so this attempt's OK
        // reply isn't skipped past by SendCommand's event budget.
        DrainPendingEvents();
        auto reply = SendCommand(add_cmd);
        if (reply && StartsWith(*reply, "OK")) {
            if (auto event = ReadUntil("P2P-GROUP-STARTED")) {
                if (auto parsed = ParseGroupStarted(*event)) {
                    sst::network::WifiDirectGroup group;
                    group.ssid = parsed->ssid;
                    group.psk = parsed->passphrase;
                    group.group_interface = parsed->interface;
                    group.group_owner_ip = kGoIpAddress;
                    group.role = kGoRole;
                    return group;
                }
                spdlog::warn("WpaWifiManager: could not parse P2P-GROUP-STARTED: {}", *event);
            } else {
                spdlog::warn("WpaWifiManager: no P2P-GROUP-STARTED (attempt {}/{})", attempt,
                             kGroupAddAttempts);
            }
        } else {
            spdlog::warn("WpaWifiManager: '{}' attempt {}/{} failed: {}", add_cmd, attempt,
                         kGroupAddAttempts, reply ? *reply : std::string{"<no reply>"});
        }
        if (attempt < kGroupAddAttempts) {
            SendCommand("P2P_GROUP_REMOVE *");  // drop any half-formed group before retrying
            std::this_thread::sleep_for(kGroupAddRetryDelay);
        }
    }
    spdlog::error("WpaWifiManager: '{}' failed after {} attempts", add_cmd, kGroupAddAttempts);
    return std::nullopt;
}

auto WpaWifiManager::StartP2pGroupOwner() -> std::optional<sst::network::WifiDirectGroup> {
    std::lock_guard lock(mtx_);
    if (!OpenCtrlSocket()) {
        return std::nullopt;
    }

    // Dedicate the radio to the GO: drop any STA association first, so the group
    // forms on its own clean channel instead of being pinned to a saved home
    // AP's channel (which starves DHCP + video on this single-radio device).
    DisableStaNetworks();

    // Clear any active group INSTANCE left over from a prior session — without
    // this a fresh P2P_GROUP_ADD never emits P2P-GROUP-STARTED. This removes the
    // running group, NOT the stored persistent network definition, so the named
    // group we reuse below survives. Best-effort (OK or FAIL both fine). Done
    // BEFORE ATTACH so its reply isn't interleaved with the P2P-GROUP-REMOVED
    // event it triggers.
    SendCommand("P2P_GROUP_REMOVE *");

    // Subscribe to unsolicited events so we capture P2P-GROUP-STARTED.
    SendCommand("ATTACH");

    // Name the GO so the SSID reads `DIRECT-XY-sst-cam-NNNN`, matching the BLE
    // advertised name. Best-effort: if the postfix can't be set the group still
    // forms (bare `DIRECT-XY`). Only relevant when CREATING a new group — a
    // reused persistent group already carries its stored SSID.
    if (!ssid_postfix_.empty()) {
        // Leading dash so the SSID reads `DIRECT-XY-sst-cam-NNNN` — wpa appends
        // the postfix raw, right after the `DIRECT-XY` stem with no separator.
        SendCommand(fmt::format("P2P_SET ssid_postfix -{}", ssid_postfix_));
    }

    // Reuse our persistent group if one is stored, else create a persistent one.
    // A persistent group stores its SSID + passphrase, so every re-form (new
    // match, BLE reconnect, transient drop) lands the phone back on the SAME
    // saved network instead of a brand-new random SSID — the whole point.
    const auto existing = FindNamedPersistentGroupId();
    const std::string add_cmd = existing ? fmt::format("P2P_GROUP_ADD persistent={}", *existing)
                                         : std::string{"P2P_GROUP_ADD persistent"};

    auto group = FormGroup(add_cmd);
    if (!group) {
        return std::nullopt;
    }

    group_interface_ = group->group_interface;
    state_ = {.mode = sst::control::WifiMode::kP2pGroupOwner,
              .connected = true,
              .ssid = group->ssid,
              .ip_address = kGoIpAddress};
    spdlog::info("WpaWifiManager::StartP2pGroupOwner {} persistent group {}",
                 existing ? "reused" : "created", *group);
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
