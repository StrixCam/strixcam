#pragma once

#include <optional>

namespace sst::control {

// Read-only probe for the connected WiFi-Direct peer's signal strength (dBm,
// always negative). The camera runs as the group owner; this is the RSSI of the
// companion app's station as the GO sees it. nullopt when no peer is connected
// or the query is unavailable — telemetry then reports "unknown" rather than a
// fabricated value.
class IWifiSignalProbe {
   public:
    IWifiSignalProbe() = default;
    virtual ~IWifiSignalProbe() = default;

    IWifiSignalProbe(const IWifiSignalProbe&) = delete;
    auto operator=(const IWifiSignalProbe&) -> IWifiSignalProbe& = delete;
    IWifiSignalProbe(IWifiSignalProbe&&) = delete;
    auto operator=(IWifiSignalProbe&&) -> IWifiSignalProbe& = delete;

    [[nodiscard]] virtual auto SampleSignalDbm() -> std::optional<int> = 0;
};

}  // namespace sst::control
