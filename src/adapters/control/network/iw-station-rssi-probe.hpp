#pragma once

#include <optional>
#include <string>

#include "app/control/ports/wifi-signal-probe.hpp"

namespace sst::adapters::control {

// Pure parser (unit-testable, no I/O): given `iw dev` output, returns the name
// of the interface whose type is "P2P-GO" — the WiFi-Direct group-owner radio
// the companion app associates with. nullopt when no GO interface is present.
auto ParseP2pGoInterface(const std::string& iw_dev_output) -> std::optional<std::string>;

// Pure parser: given `iw dev <iface> station dump` output, returns the first
// station's signal in dBm (the `signal: -57 dBm` line). nullopt when there is
// no connected station / the line is absent or malformed.
auto ParseStationSignalDbm(const std::string& station_dump_output) -> std::optional<int>;

// `iw`-backed WiFi-Direct peer-signal probe (hardware-bound — forks `iw`).
// Resolves the P2P-GO interface, then reads its connected station's RSSI. The
// reads are unprivileged on the Jetson GO (verified on-device). Bounded forks so
// a hung `iw` cannot stall the caller.
class IwStationRssiProbe final : public sst::control::IWifiSignalProbe {
   public:
    IwStationRssiProbe() = default;
    ~IwStationRssiProbe() override = default;

    IwStationRssiProbe(const IwStationRssiProbe&) = delete;
    auto operator=(const IwStationRssiProbe&) -> IwStationRssiProbe& = delete;
    IwStationRssiProbe(IwStationRssiProbe&&) = delete;
    auto operator=(IwStationRssiProbe&&) -> IwStationRssiProbe& = delete;

    [[nodiscard]] auto SampleSignalDbm() -> std::optional<int> override;
};

}  // namespace sst::adapters::control
