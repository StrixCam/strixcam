#include "app/control/services/handlers/download.handler.hpp"

#include <fmt/format.h>

#include <utility>

namespace sst::control {

// Port and TTL keep the order declared in download.handler.hpp (outside this
// change's scope); a struct/strong-type fix would have to live in that header —
// hence the swappable-parameters suppression below.
DownloadHandler::DownloadHandler(
    sst::network::DownloadServer& downloads, std::string group_owner_ip,
    std::uint32_t download_port,  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok:
                                  // distinct widths (u32 port vs u64 ttl) + self-documenting names;
                                  // no natural non-adjacent reorder
    std::uint64_t token_ttl_seconds)
    : downloads_(downloads),
      group_owner_ip_(std::move(group_owner_ip)),
      download_port_(download_port),
      token_ttl_seconds_(token_ttl_seconds) {}

auto DownloadHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kListRecordings, sst_cam::Command::kDownloadRequest};
}

auto DownloadHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    if (cmd.payload_case() == sst_cam::Command::kDownloadRequest) {
        return HandleDownloadRequest(cmd.download_request());
    }
    return HandleList();
}

auto DownloadHandler::HandleList() -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* list = resp.mutable_recording_list();
    for (const auto& rec : downloads_.Enumerate()) {
        auto* meta = list->add_recordings();
        meta->set_id(rec.recording_id);
        meta->set_size_bytes(rec.size_bytes);
        meta->set_started_at(rec.started_at_unix);
        meta->set_duration_s(rec.duration_s);
        meta->set_thumbnail_id(rec.thumbnail_id);
        meta->set_sport(rec.sport);
        meta->set_teams(rec.teams);
        // Raw dual-camera identity. Joint invariant (proto contract): set all
        // three together when raw, leave all absent for final recordings.
        if (rec.is_raw) {
            meta->set_is_raw(true);
            meta->set_camera_index(rec.camera_index);
            meta->set_capture_group_id(rec.capture_group_id);
        }
    }
    return resp;
}

auto DownloadHandler::HandleDownloadRequest(const sst_cam::DownloadRequestCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    auto token = downloads_.MintToken(cmd.recording_id(), token_ttl_seconds_);
    if (!token) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("recording not found: " + cmd.recording_id());
        return resp;
    }

    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* out = resp.mutable_download_token();
    out->set_recording_id(token->recording_id);
    out->set_http_url(fmt::format("http://{}:{}/recordings/{}", group_owner_ip_, download_port_,
                                  token->recording_id));
    out->set_auth_token(token->token);
    out->set_expires_at(token->expires_at_unix);
    return resp;
}

}  // namespace sst::control
