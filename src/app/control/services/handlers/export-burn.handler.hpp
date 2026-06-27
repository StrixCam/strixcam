#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/export/services/export-job-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles ExportOverlayedCommand + PollExportCommand (#6 F6c). ExportOverlayed
// queues a background overlay burn and returns a job id; PollExport reports the
// job state and, when ready, a download token for the burned L2.
//
// HARD INVARIANT: a burn is refused while a match is live (recording and/or
// streaming) with LIVE_SESSION_ACTIVE — the software x264 burn would contend
// with the live encode on the no-NVENC Orin Nano and risk the broadcast.
class ExportBurnHandler final : public ICommandHandler {
   public:
    // True while a match is live (is_recording || is_streaming).
    using LiveGate = std::function<bool()>;

    ExportBurnHandler(sst::exportjob::ExportJobManager& manager, LiveGate is_live,
                      std::string group_owner_ip, std::uint32_t download_port);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleExport(const sst_cam::ExportOverlayedCommand& cmd) -> sst_cam::CommandResponse;
    auto HandlePoll(const sst_cam::PollExportCommand& cmd) -> sst_cam::CommandResponse;

    sst::exportjob::ExportJobManager& manager_;
    LiveGate is_live_;
    std::string group_owner_ip_;
    std::uint32_t download_port_;
};

}  // namespace sst::control
