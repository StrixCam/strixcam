#include "adapters/control/network/iw-station-rssi-probe.hpp"

#include <charconv>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "adapters/control/network/subprocess.hpp"

namespace sst::adapters::control {

namespace {

constexpr std::string_view kInterfaceToken = "Interface ";
constexpr std::string_view kGoType = "type P2P-GO";
constexpr std::string_view kSignalToken = "signal:";

// Trims leading ASCII whitespace (iw indents with tabs/spaces).
auto LStrip(std::string_view line) -> std::string_view {
    const std::size_t start = line.find_first_not_of(" \t");
    return start == std::string_view::npos ? std::string_view{} : line.substr(start);
}

// Parses the first signed integer found in `text` (skips a leading label).
auto FirstInt(std::string_view text) -> std::optional<int> {
    std::size_t pos = 0;
    while (pos < text.size() && text[pos] != '-' && (text[pos] < '0' || text[pos] > '9')) {
        ++pos;
    }
    if (pos >= text.size()) {
        return std::nullopt;
    }
    int value = 0;
    const auto* begin = text.data() + pos;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr == begin) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

auto ParseP2pGoInterface(const std::string& iw_dev_output) -> std::optional<std::string> {
    std::istringstream stream(iw_dev_output);
    std::string line;
    std::string current_iface;
    while (std::getline(stream, line)) {
        const std::string_view stripped = LStrip(line);
        if (stripped.substr(0, kInterfaceToken.size()) == kInterfaceToken) {
            current_iface = std::string(LStrip(stripped.substr(kInterfaceToken.size())));
        } else if (stripped == kGoType && !current_iface.empty()) {
            return current_iface;
        }
    }
    return std::nullopt;
}

auto ParseStationSignalDbm(const std::string& station_dump_output) -> std::optional<int> {
    std::istringstream stream(station_dump_output);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string_view stripped = LStrip(line);
        if (stripped.substr(0, kSignalToken.size()) == kSignalToken) {
            return FirstInt(stripped.substr(kSignalToken.size()));
        }
    }
    return std::nullopt;
}

auto IwStationRssiProbe::SampleSignalDbm() -> std::optional<int> {
    const CaptureResult dev = CaptureBounded({"iw", "dev"}, kQueryTimeout);
    const std::optional<std::string> iface = ParseP2pGoInterface(dev.output);
    if (!iface) {
        return std::nullopt;
    }
    const CaptureResult dump =
        CaptureBounded({"iw", "dev", *iface, "station", "dump"}, kQueryTimeout);
    return ParseStationSignalDbm(dump.output);
}

}  // namespace sst::adapters::control
