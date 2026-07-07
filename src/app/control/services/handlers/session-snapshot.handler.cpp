#include "app/control/services/handlers/session-snapshot.handler.hpp"

#include <utility>

#include "app/control/services/handlers/match-state-mapping.hpp"
#include "domain/session/models/session-phase.hpp"
#include "domain/session/models/session-state.hpp"
#include "domain/session/models/session-summary.hpp"

namespace sst::control {

using sst::session::SessionEndReason;
using sst::session::SessionPhase;
using sst::session::SessionState;

namespace {

auto MapPhase(SessionPhase phase) -> sst_cam::SessionPhase {
    switch (phase) {
        case SessionPhase::kIdle:
            return sst_cam::SESSION_IDLE;
        case SessionPhase::kConfigured:
            return sst_cam::SESSION_CONFIGURED;
        case SessionPhase::kReady:
            return sst_cam::SESSION_READY;
        case SessionPhase::kRecording:
            return sst_cam::SESSION_RECORDING;
        case SessionPhase::kFinalizing:
            return sst_cam::SESSION_FINALIZING;
    }
    return sst_cam::SESSION_PHASE_UNKNOWN;
}

auto MapEndReason(SessionEndReason reason) -> sst_cam::SessionEndReason {
    switch (reason) {
        case SessionEndReason::kAppStop:
            return sst_cam::SESSION_END_APP_STOP;
        case SessionEndReason::kAutoStop:
            return sst_cam::SESSION_END_AUTO_STOP;
        case SessionEndReason::kCameraFailure:
            return sst_cam::SESSION_END_CAMERA_FAILURE;
        case SessionEndReason::kReboot:
            return sst_cam::SESSION_END_REBOOT;
    }
    return sst_cam::SESSION_END_UNKNOWN;
}

// Evaluate a health provider; unset provider or nullopt reading leaves the wire
// field absent (has_ false) so consumers render "unreported", never a
// fabricated OK.
auto SetHealth(const SessionSnapshotHandler::HealthProvider& provider,
               const std::function<void(sst_cam::CameraHealth)>& set) -> void {
    if (!provider) {
        return;
    }
    if (const auto health = provider()) {
        set(*health);
    }
}

}  // namespace

SessionSnapshotHandler::SessionSnapshotHandler(sst::session::ISessionManager& session,
                                               sst::decision::ManualCameraState& active_camera,
                                               sst::streaming::PreviewLayoutState& preview_layout,
                                               Providers providers, Clock now_epoch_ms)
    : session_(session),
      active_camera_(active_camera),
      preview_layout_(preview_layout),
      providers_(std::move(providers)),
      now_epoch_ms_(std::move(now_epoch_ms)) {}

auto SessionSnapshotHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kGetSessionSnapshot};
}

auto SessionSnapshotHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    const SessionState state = session_.Snapshot();

    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    auto* snap = resp.mutable_session_snapshot();
    snap->set_session_phase(MapPhase(state.phase));

    // Session-scoped UI selections. The firmware always holds a concrete
    // selection (defaults on boot, the surviving one after a disconnect), so
    // both are always reported — the app adopts them instead of resetting.
    snap->set_active_camera_index(active_camera_.Get());
    snap->set_preview_layout(preview_layout_.Get() == sst::streaming::PreviewLayout::kSideBySide
                                 ? sst_cam::PREVIEW_LAYOUT_SIDE_BY_SIDE
                                 : sst_cam::PREVIEW_LAYOUT_SINGLE);

    // Activity flags — same sources DeviceHandler's telemetry reads, so the
    // handshake and the 1 Hz poll can never disagree.
    snap->set_is_recording(providers_.is_recording && providers_.is_recording());
    snap->set_is_streaming(providers_.is_streaming && providers_.is_streaming());

    // Monotonic recording clock — present only while a recording runs (the
    // contract pins "absent when not recording"; epoch math is never used, the
    // device wall clock may predate SetDeviceTimeCommand).
    if (state.phase == SessionPhase::kRecording) {
        snap->set_recording_elapsed_seconds(state.recording_elapsed_seconds);
    }

    // Absolute match state (incl. fields 9-11) — absent when no session config
    // was ever pushed (nothing to reconcile against).
    if (state.config) {
        FillMatchState(state, now_epoch_ms_ ? now_epoch_ms_() : 0, snap->mutable_match_state());
    }

    SetHealth(providers_.camera0_health,
              [snap](sst_cam::CameraHealth health) { snap->set_camera0_health(health); });
    SetHealth(providers_.camera1_health,
              [snap](sst_cam::CameraHealth health) { snap->set_camera1_health(health); });

    // Last-session summary: only when idle AND a previous session exists —
    // never zero-filled for a first-ever connect.
    if (state.phase == SessionPhase::kIdle && state.last_summary) {
        auto* last = snap->mutable_last_session();
        last->set_match_uuid(state.last_summary->match_uuid);
        last->set_end_reason(MapEndReason(state.last_summary->end_reason));
        last->set_end_clock_seconds(state.last_summary->end_clock_seconds);
        last->set_file_valid(state.last_summary->file_valid);
    }

    snap->set_wifi_group_up(state.wifi_group_up);
    return resp;
}

}  // namespace sst::control
