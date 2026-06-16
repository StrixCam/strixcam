#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/storage/ports/raw-capture-sink.hpp"
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
    explicit RawCaptureHandler(sst::storage::IRawCaptureSink& sink);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::storage::IRawCaptureSink& sink_;
};

}  // namespace sst::control
