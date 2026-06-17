#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/session/ports/session-manager.hpp"
#include "app/storage/ports/recording-service.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles RecordingControlCommand (START / STOP / PAUSE / RESUME). START reads
// the app-supplied output paths from the session config and opens one
// continuous MP4; STOP finalizes it (+ thumbnail). Lifecycle ordering is gated
// by the SessionManager.
class RecordingHandler final : public ICommandHandler {
   public:
    RecordingHandler(sst::session::ISessionManager& session,
                     sst::storage::IRecordingService& recording);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::storage::IRecordingService& recording_;
};

}  // namespace sst::control
