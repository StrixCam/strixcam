#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/control/ports/system-stats.hpp"
#include "app/control/ports/wifi-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/device.hpp"

namespace sst::control {

// Answers the app's two polled queries (R7 — the camera never pushes
// unsolicited data; the app always initiates):
//   - GetDeviceInfo  : identity + protocol_version (version handshake, R8)
//   - GetTelemetry   : a fresh SystemStats snapshot + recording/streaming flags
class DeviceHandler final : public ICommandHandler {
   public:
    using FlagProvider = std::function<bool()>;
    // Reports the live WiFi state so telemetry reflects the actual P2P-GO link
    // (not a hardcoded UNKNOWN) — keeps the app's wifi indicator coherent.
    using WifiStateProvider = std::function<sst::control::WifiState()>;
    // Reports the connected WiFi-Direct peer's RSSI (dBm); nullopt → "unknown".
    using SignalProvider = std::function<std::optional<int>()>;
    // Frame-truth per-camera health (U3). Unset provider — or a nullopt
    // reading — leaves the wire field ABSENT (has_ false): "unreported",
    // never a fabricated OK. Same seam shape as the session snapshot's.
    using HealthProvider = std::function<std::optional<sst_cam::CameraHealth>()>;

    // The live telemetry sources, grouped and named. Three of these are the same
    // FlagProvider type and are not adjacent, so passing them positionally invited
    // a silent transposition bug (report the wrong flag, compiles clean). Callers
    // wire them with designated initializers instead:
    //   DeviceHandler{dev, stats, {.is_recording = ..., .wifi_signal_dbm = ...}}
    struct Providers {
        FlagProvider is_recording;
        FlagProvider is_streaming;
        WifiStateProvider wifi_state;
        // True when the camera has an internet uplink (default route). Distinct
        // from the WiFi-Direct GO link (link-local).
        FlagProvider internet_reachable;
        // Connected peer RSSI in dBm; nullopt when no peer / unavailable.
        SignalProvider wifi_signal_dbm;
        // Per-camera frame-truth health (DeviceTelemetry 15/16) — consumed by
        // the app's 1 Hz poll for the live health indicator.
        HealthProvider camera0_health;
        HealthProvider camera1_health;
    };

    DeviceHandler(sst::config::DeviceData device, ISystemStats& stats, Providers providers);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    [[nodiscard]] auto HandleDeviceInfo() const -> sst_cam::CommandResponse;
    auto HandleTelemetry() -> sst_cam::CommandResponse;

    sst::config::DeviceData device_;
    ISystemStats& stats_;
    Providers providers_;
};

}  // namespace sst::control
