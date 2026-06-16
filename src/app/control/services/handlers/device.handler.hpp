#pragma once

#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/control/ports/system-stats.hpp"
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

    DeviceHandler(sst::config::DeviceData device, ISystemStats& stats, FlagProvider is_recording,
                  FlagProvider is_streaming, FlagProvider is_raw_capturing);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleDeviceInfo() const -> sst_cam::CommandResponse;
    auto HandleTelemetry() -> sst_cam::CommandResponse;

    sst::config::DeviceData device_;
    ISystemStats& stats_;
    FlagProvider is_recording_;
    FlagProvider is_streaming_;
    FlagProvider is_raw_capturing_;
};

}  // namespace sst::control
