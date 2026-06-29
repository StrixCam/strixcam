#include "app/control/services/handlers/session.handler.hpp"

#include "domain/session/models/session-config.hpp"

namespace sst::control {

SessionHandler::SessionHandler(sst::session::ISessionManager& session,
                               sst::overlay::OverlayController& controller)
    : session_(session), controller_(controller) {}

auto SessionHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kPushSessionConfig};
}

auto SessionHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto& msg = cmd.push_session_config();

    sst::session::SessionConfig config;
    config.match_uuid = msg.match_uuid();
    config.user_uuid = msg.user_uuid();
    config.sport = msg.sport();
    config.num_periods = msg.num_periods();
    config.period_length_seconds = msg.period_length_seconds();
    config.rtmp_url = msg.has_rtmp_url() ? msg.rtmp_url() : "";
    config.stream_key = msg.has_stream_key() ? msg.stream_key() : "";
    config.video_output_path = msg.video_output_path();
    config.thumbnail_output_path = msg.thumbnail_output_path();
    config.team_a_id = msg.team_a_id();
    config.team_b_id = msg.team_b_id();
    config.team_a_name = msg.team_a_name();
    config.team_b_name = msg.team_b_name();
    config.team_a_color_hex = msg.team_a_color_hex();
    config.team_b_color_hex = msg.team_b_color_hex();

    sst_cam::CommandResponse resp;
    if (session_.ApplySessionConfig(config)) {
        // Configuring a match clears the overlay: a previous match's scoreboard
        // is removed from the live preview, and the board only reappears at
        // kickoff (no active match -> no overlay).
        controller_.Clear();
        resp.set_status(sst_cam::ResponseStatus::OK);
    } else {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message(
            "session config rejected: WiFi Direct group must be up first (out of order)");
    }
    return resp;
}

}  // namespace sst::control
