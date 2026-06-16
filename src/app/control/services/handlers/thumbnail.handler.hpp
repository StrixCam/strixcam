#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/pipeline/ports/frame-snapshot-source.hpp"
#include "app/storage/ports/jpeg-encoder.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Answers ThumbnailRequest with an in-memory JPEG of the latest pipeline frame.
// Grabs a snapshot from the frame-snapshot source, encodes it to JPEG bytes via
// the JPEG encoder (honoring the requested width/height/quality), and returns a
// ThumbnailResponse payload. When no frame is available (camera idle / pipeline
// not started) it returns ResponseStatus::ERROR — never an empty-OK.
class ThumbnailHandler final : public ICommandHandler {
   public:
    using Clock = std::function<std::uint64_t()>;

    ThumbnailHandler(sst::pipeline::IFrameSnapshotSource& snapshot,
                     sst::storage::IJpegEncoder& encoder, Clock now_ms);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::pipeline::IFrameSnapshotSource& snapshot_;
    sst::storage::IJpegEncoder& encoder_;
    Clock now_ms_;
};

}  // namespace sst::control
