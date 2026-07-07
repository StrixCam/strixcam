#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/streaming/models/preview-layout.hpp"

namespace sst::control {

// Answers GetSessionSnapshotCommand — the reconnect-handshake read of the
// firmware's ACTUAL state (proto §9b). Pure read, no side effects: the app
// rehydrates its UI from this instead of force-resetting to defaults. Assembled
// from the orthogonal session axes (phase / wifi_group_up), the session-scoped
// UI selections (active camera, preview layout — they survive a disconnect and
// reset only at session end), the same activity-flag sources telemetry reads,
// the monotonic recording clock, the absolute live match, per-camera health,
// and the last-session summary when idle.
class SessionSnapshotHandler final : public ICommandHandler {
   public:
    using FlagProvider = std::function<bool()>;
    // Frame-truth per-camera health (state-health cycle U3 seam). An unset
    // provider — or a nullopt return — leaves the wire field ABSENT (has_
    // false): "unreported", never a fabricated OK. The health monitor (U3)
    // fills these; until it exists main.cpp wires nothing here.
    using HealthProvider = std::function<std::optional<sst_cam::CameraHealth>()>;
    // Wall-clock epoch ms for MatchState.updated_at.
    using Clock = std::function<std::uint64_t()>;

    // Live snapshot sources, grouped and named (same designated-initializer
    // discipline as DeviceHandler::Providers — several share a type, so
    // positional wiring invites silent transposition).
    struct Providers {
        FlagProvider is_recording;
        FlagProvider is_streaming;
        HealthProvider camera0_health;
        HealthProvider camera1_health;
    };

    SessionSnapshotHandler(sst::session::ISessionManager& session,
                           sst::decision::ManualCameraState& active_camera,
                           sst::streaming::PreviewLayoutState& preview_layout, Providers providers,
                           Clock now_epoch_ms);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::decision::ManualCameraState& active_camera_;
    sst::streaming::PreviewLayoutState& preview_layout_;
    Providers providers_;
    Clock now_epoch_ms_;
};

}  // namespace sst::control
