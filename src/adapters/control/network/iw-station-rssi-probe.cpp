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

// Trims ASCII whitespace from both ends (iw indents with tabs/spaces; getline
// leaves a trailing '\r' on CRLF input, which would break the exact type match).
auto Trim(std::string_view line) -> std::string_view {
    const std::size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = line.find_last_not_of(" \t\r\n");
    return line.substr(start, end - start + 1);
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
        const std::string_view stripped = Trim(line);
        if (stripped.substr(0, kInterfaceToken.size()) == kInterfaceToken) {
            current_iface = std::string(Trim(stripped.substr(kInterfaceToken.size())));
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
        const std::string_view stripped = Trim(line);
        // Match "signal:" exactly, not the neighbouring "signal avg:" line.
        if (stripped.substr(0, kSignalToken.size()) == kSignalToken) {
            const std::optional<int> dbm = FirstInt(stripped.substr(kSignalToken.size()));
            // A real WiFi RSSI is always negative; a non-negative parse means a
            // malformed/garbage line, so report unknown rather than a bogus value.
            if (dbm && *dbm >= 0) {
                return std::nullopt;
            }
            return dbm;
        }
    }
    return std::nullopt;
}

auto IwStationRssiProbe::SampleSignalDbm() -> std::optional<int> {
    // Ignore output from a bounded run that timed out / exited non-zero: the child
    // is SIGKILLed mid-write, and parsing the partial stdout could mis-read a
    // truncated line. Report unknown instead.
    const CaptureResult dev = CaptureBounded({"iw", "dev"}, kQueryTimeout);
    if (!dev.ok) {
        return std::nullopt;
    }
    const std::optional<std::string> iface = ParseP2pGoInterface(dev.output);
    if (!iface) {
        return std::nullopt;
    }
    const CaptureResult dump =
        CaptureBounded({"iw", "dev", *iface, "station", "dump"}, kQueryTimeout);
    if (!dump.ok) {
        return std::nullopt;
    }
    return ParseStationSignalDbm(dump.output);
}

}  // namespace sst::adapters::control
