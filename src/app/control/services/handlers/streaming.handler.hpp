#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles StreamingControl (START/STOP) and SetStreamingConfig. START pushes one
// RTMP egress to the app-supplied destination (or the stored config's custom
// URL); egress goes out over cellular per the routing split (R22). The RTSP
// preview is always-on over WiFi Direct and is managed separately (U14 wiring).
class StreamingHandler final : public ICommandHandler {
   public:
    explicit StreamingHandler(sst::streaming::IStreamingService& streaming);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleControl(const sst_cam::StreamingControlCommand& cmd) -> sst_cam::CommandResponse;
    auto HandleSetConfig(const sst_cam::SetStreamingConfigCommand& cmd) -> sst_cam::CommandResponse;

    sst::streaming::IStreamingService& streaming_;

    // Single egress stream for the contract's one-destination model.
    static constexpr std::int64_t kEgressStreamId = 1;

    std::mutex mtx_;
    std::string configured_rtmp_url_;  // from SetStreamingConfig (fallback dest)
};

}  // namespace sst::control
