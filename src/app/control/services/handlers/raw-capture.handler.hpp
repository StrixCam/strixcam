#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/control/services/handlers/health-gate.hpp"
#include "app/raw_capture/ports/raw-capture-sink.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles RawCaptureControlCommand (independent raw dual-camera capture, distinct
// from RecordingControlCommand). START opens both per-camera raw files stamped
// with the APP-minted capture_group_id (the firmware never mints its own — see
// the proto contract); STOP closes them. Only START/STOP are honored: PAUSE and
// RESUME return UNSUPPORTED, and an absent/UNKNOWN action returns ERROR — never
// treated as START. Runs concurrently with final recording + streaming.
class RawCaptureHandler final : public ICommandHandler {
   public:
    // `health_gate` refuses START with DEVICE_INOPERABLE while any camera is
    // not OK (U3); STOP is never gated. Default gates nothing.
    explicit RawCaptureHandler(sst::raw_capture::IRawCaptureSink& sink,
                               StartHealthGate health_gate = {});

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::raw_capture::IRawCaptureSink& sink_;
    StartHealthGate health_gate_;
};

}  // namespace sst::control
