#include "app/control/services/handlers/match.handler.hpp"

#include <map>
#include <string>
#include <utility>

#include "app/overlay/binding-sync.hpp"
#include "domain/session/models/live-match.hpp"

namespace sst::control {

using sst::session::LiveMatch;
using sst::session::MatchSegment;

MatchHandler::MatchHandler(sst::session::ISessionManager& session,
                           sst::overlay::OverlayController& controller, Clock now_ms)
    : session_(session), controller_(controller), now_ms_(std::move(now_ms)) {}

auto MatchHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kMatchControl, sst_cam::Command::kScoreUpdate,
            sst_cam::Command::kBannerEvent};
}

auto MatchHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    switch (cmd.payload_case()) {
        case sst_cam::Command::kScoreUpdate:
            return HandleScoreUpdate(cmd.score_update());
        case sst_cam::Command::kMatchControl:
            return HandleMatchControl(cmd.match_control());
        case sst_cam::Command::kBannerEvent:
            return HandleBannerEvent(cmd.banner_event());
        default:
            break;
    }
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::ERROR);
    resp.set_error_message("match handler: unexpected command");
    return resp;
}

auto MatchHandler::SyncOverlay() -> void {
    controller_.SetBindingData(sst::overlay::BuildBindingData(session_.Snapshot()));
    controller_.Refresh(now_ms_ ? now_ms_() : 0);
}

auto MatchHandler::HandleScoreUpdate(const sst_cam::ScoreUpdateCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    const auto state = session_.Snapshot();
    if (!state.config) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("score update with no active session");
        return resp;
    }
    const bool is_home = cmd.team_id() == state.config->team_a_id;
    const bool is_away = cmd.team_id() == state.config->team_b_id;
    if (!is_home && !is_away) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("score update: unknown team_id");
        return resp;
    }

    const std::int32_t delta = cmd.delta();
    const bool applied = session_.ApplyMatchUpdate([&](LiveMatch& match) {
        std::uint32_t& score = is_home ? match.score_a : match.score_b;
        // Clamp at zero — a negative delta can't drive the score below 0.
        if (delta < 0 && static_cast<std::uint32_t>(-delta) > score) {
            score = 0;
        } else {
            score = static_cast<std::uint32_t>(static_cast<std::int64_t>(score) + delta);
        }
    });
    if (!applied) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("score update rejected: no active session");
        return resp;
    }

    SyncOverlay();
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

auto MatchHandler::HandleMatchControl(const sst_cam::MatchControlCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    const std::uint32_t period = cmd.period();
    const auto action = cmd.action();

    const bool applied = session_.ApplyMatchUpdate([&](LiveMatch& match) {
        switch (action) {
            case sst_cam::MATCH_KICKOFF:
                match.period = period > 0 ? period : 1;
                match.clock_seconds = 0;
                match.clock_running = true;
                match.segment = MatchSegment::kInPlay;
                break;
            case sst_cam::MATCH_PERIOD_START:
                match.period = period > 0 ? period : match.period;
                match.clock_running = true;
                match.segment = MatchSegment::kInPlay;
                break;
            case sst_cam::MATCH_PERIOD_END:
                match.clock_running = false;
                match.segment = MatchSegment::kHalfTime;
                break;
            case sst_cam::MATCH_FINAL_WHISTLE:
                match.clock_running = false;
                match.segment = MatchSegment::kFullTime;
                break;
            case sst_cam::MATCH_CLOCK_PAUSE:
                match.clock_running = false;
                break;
            case sst_cam::MATCH_CLOCK_RESUME:
                match.clock_running = true;
                break;
            default:
                break;
        }
    });
    if (!applied) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("match control rejected: no active session");
        return resp;
    }

    SyncOverlay();
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

auto MatchHandler::HandleBannerEvent(const sst_cam::BannerEventCommand& cmd)
    -> sst_cam::CommandResponse {
    std::map<std::string, std::string> params;
    for (const auto& [key, value] : cmd.params()) {
        params.emplace(key, value);
    }
    const std::uint64_t now = now_ms_ ? now_ms_() : 0;
    // Unknown template_id is a no-op (the app may target a template this layout
    // doesn't define) — still a successful, defined response.
    controller_.ActivateBanner(cmd.template_id(), params, cmd.duration_s(), now);
    controller_.Refresh(now);

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

auto MatchHandler::TickClock() -> void {
    const bool advanced = session_.ApplyMatchUpdate([](LiveMatch& match) {
        if (match.clock_running) {
            ++match.clock_seconds;
        }
    });
    if (advanced) {
        SyncOverlay();
    }
}

}  // namespace sst::control
