#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles SetMatchStateCommand — the post-disconnect reconciliation verb:
// an ABSOLUTE overwrite of the live match (proto §9b). Every wire field is
// optional; only fields present (has_*) are written, absent fields stay
// untouched, so a partial set is legal. This complements — never replaces —
// the live incremental ScoreUpdateCommand path: deltas replayed after a
// connection gap double-apply, absolute values don't. A successful apply
// refreshes the overlay so the on-frame scoreboard reflects the reconciled
// state immediately (not one clock tick later).
class SetMatchStateHandler final : public ICommandHandler {
   public:
    using Clock = std::function<std::uint64_t()>;

    SetMatchStateHandler(sst::session::ISessionManager& session,
                         sst::overlay::OverlayController& controller, Clock now_ms);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::overlay::OverlayController& controller_;
    Clock now_ms_;
};

}  // namespace sst::control
