#include "app/control/services/handlers/export-burn.handler.hpp"

#include <fmt/format.h>

#include <utility>

namespace sst::control {

namespace {

auto ToProtoState(sst::exportjob::ExportState state) -> sst_cam::ExportJobState {
    switch (state) {
        case sst::exportjob::ExportState::kPending:
            return sst_cam::EXPORT_JOB_PENDING;
        case sst::exportjob::ExportState::kRunning:
            return sst_cam::EXPORT_JOB_RUNNING;
        case sst::exportjob::ExportState::kReady:
            return sst_cam::EXPORT_JOB_READY;
        case sst::exportjob::ExportState::kFailed:
            return sst_cam::EXPORT_JOB_FAILED;
    }
    return sst_cam::EXPORT_JOB_UNKNOWN;
}

}  // namespace

ExportBurnHandler::ExportBurnHandler(sst::exportjob::ExportJobManager& manager, LiveGate is_live,
                                     std::string group_owner_ip, std::uint32_t download_port)
    : manager_(manager),
      is_live_(std::move(is_live)),
      group_owner_ip_(std::move(group_owner_ip)),
      download_port_(download_port) {}

auto ExportBurnHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kExportOverlayed, sst_cam::Command::kPollExport};
}

auto ExportBurnHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    switch (cmd.payload_case()) {
        case sst_cam::Command::kExportOverlayed:
            return HandleExport(cmd.export_overlayed());
        case sst_cam::Command::kPollExport:
            return HandlePoll(cmd.poll_export());
        default:
            break;
    }
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::ERROR);
    resp.set_error_message("export handler: unexpected command");
    return resp;
}

auto ExportBurnHandler::HandleExport(const sst_cam::ExportOverlayedCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    // HARD INVARIANT — never burn while a match is live.
    if (is_live_ && is_live_()) {
        resp.set_status(sst_cam::ResponseStatus::LIVE_SESSION_ACTIVE);
        resp.set_error_message("cannot export while a match is live");
        return resp;
    }
    // Pass the live gate through so the worker re-checks it just before encoding
    // (the check above is only a point-in-time read).
    const std::string job_id = manager_.RequestExport(cmd.recording_id(), is_live_);
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* job = resp.mutable_export_job();
    job->set_job_id(job_id);
    job->set_state(sst_cam::EXPORT_JOB_PENDING);
    return resp;
}

auto ExportBurnHandler::HandlePoll(const sst_cam::PollExportCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* job = resp.mutable_export_job();
    job->set_job_id(cmd.job_id());

    const auto view = manager_.Poll(cmd.job_id());
    if (!view) {
        // No such job (e.g. firmware restarted since the request).
        job->set_state(sst_cam::EXPORT_JOB_UNKNOWN);
        return resp;
    }
    job->set_state(ToProtoState(view->state));
    if (view->state == sst::exportjob::ExportState::kFailed) {
        resp.set_error_message(view->error);
    }
    if (view->state == sst::exportjob::ExportState::kReady && view->token) {
        auto* token = job->mutable_token();
        token->set_recording_id(view->token->recording_id);
        token->set_http_url(fmt::format("http://{}:{}/recordings/{}", group_owner_ip_,
                                        download_port_, view->token->recording_id));
        token->set_auth_token(view->token->token);
        token->set_expires_at(view->token->expires_at_unix);
    }
    return resp;
}

}  // namespace sst::control
