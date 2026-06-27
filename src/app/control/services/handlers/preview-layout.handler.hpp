#pragma once

#include <cstdint>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"
#include "domain/streaming/models/preview-layout.hpp"

namespace sst::control {

// Handles SetPreviewLayoutCommand (#6 F6d): flips the shared PreviewLayoutState
// the pipeline consumer reads each tick, and replies with the preview stream
// geometry so the app can size its preview box. SINGLE shows the active camera
// (overlay baked in); SIDE_BY_SIDE shows cam0 | cam1 composited clean. The RTSP
// geometry is fixed (both layouts share the output canvas), so the response
// reports the same width/height for both — the switch is graph-stable.
class PreviewLayoutHandler final : public ICommandHandler {
   public:
    PreviewLayoutHandler(sst::streaming::PreviewLayoutState& state, std::uint32_t stream_width,
                         std::uint32_t stream_height);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::streaming::PreviewLayoutState& state_;
    std::uint32_t stream_width_;
    std::uint32_t stream_height_;
};

}  // namespace sst::control
