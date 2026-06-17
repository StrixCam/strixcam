#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles PushSessionConfigCommand: snapshots the app-supplied session config
// into the SessionManager (which prepares output dirs and advances the
// lifecycle to Configured). Ordering is enforced by the SM — a config pushed
// before the WiFi group is up is rejected with status=ERROR.
class SessionHandler final : public ICommandHandler {
   public:
    explicit SessionHandler(sst::session::ISessionManager& session);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
};

}  // namespace sst::control
