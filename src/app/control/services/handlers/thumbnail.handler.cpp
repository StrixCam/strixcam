#include "app/control/services/handlers/thumbnail.handler.hpp"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>

namespace sst::control {

ThumbnailHandler::ThumbnailHandler(sst::pipeline::IFrameSnapshotSource& snapshot,
                                   sst::storage::IJpegEncoder& encoder, Clock now_ms)
    : snapshot_(snapshot), encoder_(encoder), now_ms_(std::move(now_ms)) {}

auto ThumbnailHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kThumbnail};
}

auto ThumbnailHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const sst_cam::ThumbnailRequest& req = cmd.thumbnail();

    sst_cam::CommandResponse resp;

    auto frame = snapshot_.GrabLatest();
    if (!frame) {
        // Real error, not a skeleton: the camera has produced no frame yet.
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("no frame available for thumbnail");
        spdlog::info("ThumbnailHandler: no frame available");
        return resp;
    }

    auto jpeg = encoder_.Encode(*frame, sst::common::OutputSize{req.width(), req.height()},
                                req.quality());
    if (!jpeg || jpeg->empty()) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("thumbnail JPEG encode failed");
        spdlog::warn("ThumbnailHandler: encode failed");
        return resp;
    }

    sst_cam::ThumbnailResponse* thumb = resp.mutable_thumbnail();
    thumb->set_jpeg_bytes(std::string(jpeg->begin(), jpeg->end()));
    thumb->set_capture_timestamp(now_ms_ ? now_ms_() : 0);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
