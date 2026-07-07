#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/control/services/handlers/health-gate.hpp"
#include "app/overlay/ports/overlay-timeline-recorder.hpp"
#include "app/session/ports/session-manager.hpp"
#include "app/storage/ports/recording-service.hpp"
#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles RecordingControlCommand (START / STOP / PAUSE / RESUME). START reads
// the app-supplied output paths from the session config and opens one
// continuous MP4; STOP finalizes it (+ thumbnail). Lifecycle ordering is gated
// by the SessionManager.
class RecordingHandler final : public ICommandHandler {
   public:
    // Returns the current overlay monotonic clock (NowMs) — the same clock the
    // overlay handlers use, so the timeline anchor lines up with the scene
    // timestamps. May be null (timeline anchored at 0, capture still works).
    using Clock = std::function<std::uint64_t()>;

    // `health_gate` refuses START with DEVICE_INOPERABLE while any camera is
    // not OK (U3); STOP/PAUSE/RESUME are never gated. Default gates nothing.
    // `proxy` is the shared record-or-stream ref-count for the training proxy —
    // this handler drives its record leg (START/STOP take/release the hold).
    RecordingHandler(sst::session::ISessionManager& session,
                     sst::storage::IRecordingService& recording,
                     sst::overlay::IOverlayTimelineRecorder& timeline,
                     sst::storage::ProxyLifecycle& proxy, Clock now_ms,
                     StartHealthGate health_gate = {});

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::storage::IRecordingService& recording_;
    sst::overlay::IOverlayTimelineRecorder& timeline_;
    sst::storage::ProxyLifecycle& proxy_;
    Clock now_ms_;
    StartHealthGate health_gate_;
};

}  // namespace sst::control
