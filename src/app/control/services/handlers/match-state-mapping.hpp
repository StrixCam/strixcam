#pragma once

#include <cstdint>

#include "bluetooth.pb.h"
#include "domain/session/models/live-match.hpp"
#include "domain/session/models/session-state.hpp"

namespace sst::control {

// Shared LiveMatch -> wire MatchState mapping. Two handlers serve the same
// absolute match state — MatchStateHandler (GetMatchState, the app's live poll)
// and SessionSnapshotHandler (the reconnect handshake read) — so the derivation
// lives here once and the two responses can never drift.

// Map the display-only live-match segment + clock onto the contract MatchStatus.
// With no session config there is nothing the app has provisioned yet, so the
// match has not started.
inline auto DeriveMatchStatus(const sst::session::SessionState& state) -> sst_cam::MatchStatus {
    if (!state.config) {
        return sst_cam::MATCH_NOT_STARTED;
    }
    const sst::session::LiveMatch& match = state.match;
    switch (match.segment) {
        case sst::session::MatchSegment::kHalfTime:
            return sst_cam::MATCH_HALF_TIME;
        case sst::session::MatchSegment::kFullTime:
            return sst_cam::MATCH_FINISHED;
        case sst::session::MatchSegment::kInPlay:
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
// minus the locally-ticked elapsed clock, clamped at zero (the wire field is
// documented lossy past period end — elapsed_seconds carries the unclamped
// truth). Period length 0 (unset) yields 0 remaining.
inline auto DeriveTimeRemaining(const sst::session::SessionState& state) -> std::uint32_t {
    if (!state.config || state.config->period_length_seconds <= 0) {
        return 0;
    }
    const auto length = static_cast<std::uint32_t>(state.config->period_length_seconds);
    const std::uint32_t elapsed = state.match.clock_seconds;
    return elapsed >= length ? 0U : length - elapsed;
}

// Populate a wire MatchState from the session snapshot. Fields 9-11
// (elapsed_seconds / clock_running / match_uuid) are set only while a session
// config is live — absent means "no firmware match clock exists", so a
// reconciling app never adopts a zero-filled clock from an idle camera.
// elapsed_seconds is the monotonic firmware clock, deliberately NOT clamped at
// period length (unlike time_remaining_s).
inline auto FillMatchState(const sst::session::SessionState& state, std::uint64_t updated_at_ms,
                           sst_cam::MatchState* out) -> void {
    out->set_status(DeriveMatchStatus(state));
    out->set_current_period(state.match.period);
    out->set_time_remaining_s(DeriveTimeRemaining(state));
    out->set_score_a(state.match.score_a);
    out->set_score_b(state.match.score_b);
    out->set_updated_at(updated_at_ms);
    if (state.config) {
        out->set_team_a_id(state.config->team_a_id);
        out->set_team_b_id(state.config->team_b_id);
        out->set_elapsed_seconds(state.match.clock_seconds);
        out->set_clock_running(state.match.clock_running);
        out->set_match_uuid(state.config->match_uuid);
    }
}

}  // namespace sst::control
