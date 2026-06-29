#pragma once

#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles PushSessionConfigCommand: snapshots the app-supplied session config
// into the SessionManager (which prepares output dirs and advances the
// lifecycle to Configured). Ordering is enforced by the SM — a config pushed
// before the WiFi group is up is rejected with status=ERROR.
//
// On a successful config push it also clears the overlay, so configuring a (new)
// match removes any previous match's scoreboard from the live preview. The board
// only reappears at kickoff (no active match -> no overlay).
class SessionHandler final : public ICommandHandler {
   public:
    SessionHandler(sst::session::ISessionManager& session,
                   sst::overlay::OverlayController& controller);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::overlay::OverlayController& controller_;
};

}  // namespace sst::control
