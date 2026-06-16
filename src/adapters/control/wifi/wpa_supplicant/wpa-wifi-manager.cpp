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
#include <string>
#include <utility>

#include "adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp"
#include "domain/network/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::adapters::control {

namespace {

constexpr std::size_t kRecvBufSize = 4096;
constexpr auto kRecvTimeout = std::chrono::seconds{2};
constexpr const char* kGoIpAddress = "192.168.49.1";
constexpr const char* kGoRole = "GO";
constexpr int kMaxEventReads = 20;

auto StartsWith(const std::string& s, std::string_view prefix) -> bool {
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

}  // namespace

WpaWifiManager::WpaWifiManager(std::string iface, std::string ctrl_dir)
    : iface_(std::move(iface)), ctrl_dir_(std::move(ctrl_dir)) {}

WpaWifiManager::~WpaWifiManager() { CloseCtrlSocket(); }

auto WpaWifiManager::OpenCtrlSocket() -> bool {
    if (sock_ >= 0) {
        return true;
    }

    sock_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        spdlog::error("WpaWifiManager: socket() failed: {}", std::strerror(errno));
        return false;
    }

    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    local_path_ = fmt::format("/tmp/wpa_ctrl_{}-{}", ::getpid(), static_cast<int>(std::rand()));
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

    timeval tv{};
    tv.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(kRecvTimeout).count();
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    std::array<char, kRecvBufSize> buf{};
    const auto n = ::recv(sock_, buf.data(), buf.size() - 1, 0);
    if (n < 0) {
        spdlog::error("WpaWifiManager: recv after \"{}\" failed: {}", cmd, std::strerror(errno));
        return std::nullopt;
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

auto WpaWifiManager::ReadUntil(std::string_view marker) const
    -> std::optional<std::string> {
  for (int i = 0; i < kMaxEventReads; ++i) {
    std::array<char, kRecvBufSize> buf{};
    const auto n = ::recv(sock_, buf.data(), buf.size() - 1, 0);
    if (n <= 0) {
      return std::nullopt;  // timeout / closed
    }
    std::string msg(buf.data(), static_cast<std::size_t>(n));
    if (msg.find(marker) != std::string::npos) {
      return msg;
    }
  }
  return std::nullopt;
}

auto WpaWifiManager::StartP2pGroupOwner() -> std::optional<sst::network::WifiDirectGroup> {
    std::lock_guard lock(mtx_);
    if (!OpenCtrlSocket()) {
        return std::nullopt;
    }

    // Subscribe to unsolicited events so we capture P2P-GROUP-STARTED.
    SendCommand("ATTACH");

    // Form a real autonomous (non-persistent) group owner. wpa_supplicant
    // generates the SSID + passphrase.
    auto reply = SendCommand("P2P_GROUP_ADD");
    if (!reply || !StartsWith(*reply, "OK")) {
        spdlog::error("WpaWifiManager: P2P_GROUP_ADD failed: {}",
                      reply ? *reply : std::string{"<no reply>"});
        return std::nullopt;
    }

    auto event = ReadUntil("P2P-GROUP-STARTED");
    if (!event) {
        spdlog::error("WpaWifiManager: no P2P-GROUP-STARTED event received");
        return std::nullopt;
    }
    auto parsed = ParseGroupStarted(*event);
    if (!parsed) {
        spdlog::error("WpaWifiManager: could not parse P2P-GROUP-STARTED: {}", *event);
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
