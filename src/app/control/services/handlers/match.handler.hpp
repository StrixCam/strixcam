#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles the match event-push commands (MatchControl, ScoreUpdate,
// BannerEvent). The app is the score + timing authority (R20); this handler
// only reflects what the app pushes into the in-memory live match and the
// overlay. It also advances a display-only local clock between app events.
class MatchHandler final : public ICommandHandler {
   public:
    using Clock = std::function<std::uint64_t()>;

    MatchHandler(sst::session::ISessionManager& session,
                 sst::overlay::OverlayController& controller, Clock now_ms);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

    // Display-only clock tick (driven ~1 Hz by the runtime, U14). Advances the
    // local clock by one second while running and refreshes the overlay.
    auto TickClock() -> void;

   private:
    auto HandleScoreUpdate(const sst_cam::ScoreUpdateCommand& cmd) -> sst_cam::CommandResponse;
    auto HandleMatchControl(const sst_cam::MatchControlCommand& cmd) -> sst_cam::CommandResponse;
    auto HandleBannerEvent(const sst_cam::BannerEventCommand& cmd) -> sst_cam::CommandResponse;
    auto SyncOverlay() -> void;

    sst::session::ISessionManager& session_;
    sst::overlay::OverlayController& controller_;
    Clock now_ms_;
};

}  // namespace sst::control
