#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Answers GetMatchStateCommand (which the app polls) with the current
// display-only match snapshot. The app is the score + timing authority (R20);
// this handler only reflects what the app last pushed into the in-memory live
// match plus the team identities from the session config. It never invents
// authoritative state — with no active session it reports MATCH_NOT_STARTED.
class MatchStateHandler final : public ICommandHandler {
   public:
    using Clock = std::function<std::uint64_t()>;

    MatchStateHandler(sst::session::ISessionManager& session, Clock now_ms);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    Clock now_ms_;
};

}  // namespace sst::control
