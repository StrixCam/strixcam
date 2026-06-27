#include "app/control/services/handlers/preview-layout.handler.hpp"

namespace sst::control {

// NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: preview stream
// width/height; passed by name (kOverlayWidth, kOverlayHeight) at the single call site
PreviewLayoutHandler::PreviewLayoutHandler(sst::streaming::PreviewLayoutState& state,
                                           std::uint32_t stream_width, std::uint32_t stream_height)
    // NOLINTEND(bugprone-easily-swappable-parameters)
    : state_(state), stream_width_(stream_width), stream_height_(stream_height) {}

auto PreviewLayoutHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kSetPreviewLayout};
}

auto PreviewLayoutHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto requested = cmd.set_preview_layout().layout();
    const bool side_by_side = requested == sst_cam::PreviewLayout::PREVIEW_LAYOUT_SIDE_BY_SIDE;
    state_.Set(side_by_side ? sst::streaming::PreviewLayout::kSideBySide
                            : sst::streaming::PreviewLayout::kSingle);

    sst_cam::CommandResponse resp;
    auto* payload = resp.mutable_preview_layout();
    payload->set_layout(requested);
    payload->set_width(stream_width_);
    payload->set_height(stream_height_);
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
