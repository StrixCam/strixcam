#include "app/control/services/handlers/match-state.handler.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "domain/session/models/live-match.hpp"
#include "domain/session/models/session-state.hpp"

namespace sst::control {

using sst::session::LiveMatch;
using sst::session::MatchSegment;
using sst::session::SessionState;

namespace {

// Map the display-only live-match segment + clock onto the contract MatchStatus.
// With no session config there is nothing the app has provisioned yet, so the
// match has not started.
auto DeriveStatus(const SessionState& state) -> sst_cam::MatchStatus {
    if (!state.config) {
        return sst_cam::MATCH_NOT_STARTED;
    }
    const LiveMatch& match = state.match;
    switch (match.segment) {
        case MatchSegment::kHalfTime:
            return sst_cam::MATCH_HALF_TIME;
        case MatchSegment::kFullTime:
            return sst_cam::MATCH_FINISHED;
        case MatchSegment::kInPlay:
            break;
    }
    // In play: a started period that is running is ACTIVE; a started period with
    // the clock halted is PAUSED; before kickoff (no period yet) it has not
    // started.
    if (match.period == 0) {
        return sst_cam::MATCH_NOT_STARTED;
    }
    return match.clock_running ? sst_cam::MATCH_ACTIVE : sst_cam::MATCH_PAUSED;
}

// Remaining seconds in the current period from the app-configured period length
// minus the locally-ticked elapsed clock, clamped at zero. Period length 0
// (unset) yields 0 remaining.
auto DeriveTimeRemaining(const SessionState& state) -> std::uint32_t {
    if (!state.config || state.config->period_length_seconds <= 0) {
        return 0;
    }
    const auto length = static_cast<std::uint32_t>(state.config->period_length_seconds);
    const std::uint32_t elapsed = state.match.clock_seconds;
    return elapsed >= length ? 0U : length - elapsed;
}

}  // namespace

MatchStateHandler::MatchStateHandler(sst::session::ISessionManager& session, Clock now_ms)
    : session_(session), now_ms_(std::move(now_ms)) {}

auto MatchStateHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kGetMatchState};
}

auto MatchStateHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    const SessionState state = session_.Snapshot();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);

    sst_cam::MatchState* match_state = resp.mutable_match_state();
    match_state->set_status(DeriveStatus(state));
    match_state->set_current_period(state.match.period);
    match_state->set_time_remaining_s(DeriveTimeRemaining(state));
    match_state->set_score_a(state.match.score_a);
    match_state->set_score_b(state.match.score_b);
    if (state.config) {
        match_state->set_team_a_id(state.config->team_a_id);
        match_state->set_team_b_id(state.config->team_b_id);
    }
    match_state->set_updated_at(now_ms_ ? now_ms_() : 0);
    return resp;
}

}  // namespace sst::control
