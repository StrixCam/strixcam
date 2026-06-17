#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sst::adapters::control {

// Extract a key="quoted value" field from a wpa_supplicant event/status line.
// Public signature with distinct roles (haystack vs. key) consumed positionally
// by external callers; reordering would break the contract and a struct param
// would only obscure it — hence the swappable-parameters suppression below.
inline auto ParseQuotedField(
    std::string_view text,  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok: fixed
                            // (haystack, key) convention consumed positionally by external callers
    std::string_view key) -> std::optional<std::string> {
    const std::string needle = std::string(key) + "=\"";
    const auto pos = text.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + needle.size();
    const auto end = text.find('"', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(text.substr(start, end - start));
}

// The interface name in a P2P-GROUP-STARTED event is the first token after the
// event name, e.g. "P2P-GROUP-STARTED p2p-wlan0-0 GO ssid=..." -> "p2p-wlan0-0".
inline auto ParseGroupInterface(std::string_view event) -> std::optional<std::string> {
    constexpr std::string_view kTag = "P2P-GROUP-STARTED";
    auto pos = event.find(kTag);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += kTag.size();
    while (pos < event.size() && event[pos] == ' ') {
        ++pos;
    }
    auto end = pos;
    while (end < event.size() && event[end] != ' ') {
        ++end;
    }
    if (end == pos) {
        return std::nullopt;
    }
    return std::string(event.substr(pos, end - pos));
}

struct ParsedGroup {
    std::string interface;
    std::string ssid;
    std::string passphrase;
};

// Parse a P2P-GROUP-STARTED event, which carries the generated interface, SSID,
// and passphrase: "P2P-GROUP-STARTED p2p-wlan0-0 GO ssid=\"DIRECT-ab\"
// freq=2412 passphrase=\"swxyz123\" go_dev_addr=...".
inline auto ParseGroupStarted(std::string_view event) -> std::optional<ParsedGroup> {
    auto iface = ParseGroupInterface(event);
    auto ssid = ParseQuotedField(event, "ssid");
    auto passphrase = ParseQuotedField(event, "passphrase");
    if (!iface || !ssid || !passphrase) {
        return std::nullopt;
    }
    return ParsedGroup{.interface = *iface, .ssid = *ssid, .passphrase = *passphrase};
}

}  // namespace sst::adapters::control
