#include "app/control/services/handlers/set-match-state.handler.hpp"

#include <utility>

#include "app/overlay/binding-sync.hpp"
#include "domain/session/models/live-match.hpp"

namespace sst::control {

using sst::session::LiveMatch;
using sst::session::MatchSegment;

namespace {

// Wire MatchStatus -> display segment. ACTIVE/PAUSED/NOT_STARTED are all
// in-play shapes (paused-vs-active derives from clock_running, not-started from
// period == 0 — see match-state-mapping.hpp), so only the two terminal
// segments carry status information of their own.
auto SegmentFor(sst_cam::MatchStatus status) -> MatchSegment {
    switch (status) {
        case sst_cam::MATCH_HALF_TIME:
            return MatchSegment::kHalfTime;
        case sst_cam::MATCH_FINISHED:
            return MatchSegment::kFullTime;
        default:
            return MatchSegment::kInPlay;
    }
}

}  // namespace

SetMatchStateHandler::SetMatchStateHandler(sst::session::ISessionManager& session,
                                           sst::overlay::OverlayController& controller,
                                           Clock now_ms)
    : session_(session), controller_(controller), now_ms_(std::move(now_ms)) {}

auto SetMatchStateHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetMatchState};
}

auto SetMatchStateHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto& msg = cmd.set_match_state();

    // Absolute overwrite, field by field: only fields the app sent are written
    // (has_* discipline) — a partial reconcile leaves the rest of the live
    // match exactly as the firmware kept it through the disconnect.
    const bool applied = session_.ApplyMatchUpdate([&msg](LiveMatch& match) {
        if (msg.has_score_a()) {
            match.score_a = msg.score_a();
        }
        if (msg.has_score_b()) {
            match.score_b = msg.score_b();
        }
        if (msg.has_current_period()) {
            match.period = msg.current_period();
        }
        if (msg.has_elapsed_seconds()) {
            match.clock_seconds = msg.elapsed_seconds();
        }
        if (msg.has_clock_running()) {
            match.clock_running = msg.clock_running();
        }
        if (msg.has_status()) {
            match.segment = SegmentFor(msg.status());
        }
    });

    sst_cam::CommandResponse resp;
    if (!applied) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("set match state rejected: no active session");
        return resp;
    }

    // Refresh the overlay from the reconciled snapshot so the on-frame
    // scoreboard agrees with the app immediately.
    controller_.SetBindingData(sst::overlay::BuildBindingData(session_.Snapshot()));
    controller_.Refresh(now_ms_ ? now_ms_() : 0);

    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
