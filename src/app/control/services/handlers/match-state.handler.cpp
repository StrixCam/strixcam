#include "app/control/services/handlers/match-state.handler.hpp"

#include <utility>

#include "app/control/services/handlers/match-state-mapping.hpp"
#include "domain/session/models/session-state.hpp"

namespace sst::control {

using sst::session::SessionState;

MatchStateHandler::MatchStateHandler(sst::session::ISessionManager& session, Clock now_ms)
    : session_(session), now_ms_(std::move(now_ms)) {}

auto MatchStateHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kGetMatchState};
}

auto MatchStateHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    const SessionState state = session_.Snapshot();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    // Shared mapping (match-state-mapping.hpp) — also fills the state-health
    // cycle fields 9-11 (unclamped elapsed_seconds, clock_running, match_uuid)
    // while a session config is live.
    FillMatchState(state, now_ms_ ? now_ms_() : 0, resp.mutable_match_state());
    return resp;
}

}  // namespace sst::control
