#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/control/services/handlers/health-gate.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "app/streaming/ports/uplink-probe.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles StreamingControl (START/STOP) and SetStreamingConfig. START pushes one
// RTMP egress to the app-supplied destination (or the stored config's custom
// URL); egress goes out over cellular per the routing split (R22). The RTSP
// preview is always-on over WiFi Direct and is managed separately (U14 wiring).
class StreamingHandler final : public ICommandHandler {
   public:
    // `uplink_probe` is optional (nullptr disables the pre-flight): when set,
    // a cloud-stream START with no internet uplink is rejected with a clear
    // message instead of an opaque rtmp connect failure (U6 / R9).
    // `health_gate` refuses START with DEVICE_INOPERABLE while any camera is
    // not OK (U3); STOP and SetStreamingConfig are never gated.
    explicit StreamingHandler(sst::streaming::IStreamingService& streaming,
                              sst::streaming::IUplinkProbe* uplink_probe = nullptr,
                              StartHealthGate health_gate = {});

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleControl(const sst_cam::StreamingControlCommand& cmd) -> sst_cam::CommandResponse;
    auto HandleSetConfig(const sst_cam::SetStreamingConfigCommand& cmd) -> sst_cam::CommandResponse;

    sst::streaming::IStreamingService& streaming_;
    sst::streaming::IUplinkProbe* uplink_probe_;
    StartHealthGate health_gate_;

    // Single egress stream for the contract's one-destination model.
    static constexpr std::int64_t kEgressStreamId = 1;

    std::mutex mtx_;
    std::string configured_rtmp_url_;  // from SetStreamingConfig (fallback dest)
};

}  // namespace sst::control
