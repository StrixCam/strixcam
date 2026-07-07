// GetSessionSnapshot handler: the reconnect-handshake read of firmware ACTUAL
// state (state-health cycle U2, proto §9b). Pure — real SessionManager with a
// fake cleanup/store; selections + activity flags injected; no hardware.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/control/ports/system-stats.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/device.handler.hpp"
#include "app/control/services/handlers/session-snapshot.handler.hpp"
#include "app/control/services/handlers/set-device-time.handler.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/ports/session-summary-store.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/device.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/session/models/session-summary.hpp"
#include "domain/streaming/models/preview-layout.hpp"

namespace {

using sst::control::SessionSnapshotHandler;
using sst::session::LastSessionSummary;
using sst::session::SessionConfig;
using sst::session::SessionEndReason;
using sst::session::SessionManager;
using sst::session::SessionTiming;

// Scaled-down auto-stop timing for the timer scenario (same margins as the
// session-manager suite: fire wait ≤ 5 s stays stable under qemu in CI).
constexpr std::chrono::milliseconds kAutoStop{200};
constexpr std::chrono::milliseconds kConfigMinute{40};
constexpr std::chrono::seconds kFireWait{5};
constexpr std::chrono::milliseconds kPollInterval{5};
// Fixed wall-clock stamp injected as the handler's epoch clock.
constexpr std::uint64_t kEpochStamp = 1'750'000'000'000ULL;
// Match clock reading used by the recording snapshot scenario.
constexpr std::uint32_t kMatchClockSeconds = 1234;
// Configured period length for the fixtures (seconds).
constexpr std::int32_t kPeriodLengthSeconds = 600;
// Orphaned session's match clock preloaded by the reboot scenario (seconds).
constexpr std::uint32_t kRebootEndClock = 500;

auto WaitFor(const std::function<bool()>& pred) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + kFireWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return pred();
}

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override { return true; }
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}
};

class FakeStore final : public sst::session::ISessionSummaryStore {
   public:
    auto Persist(const LastSessionSummary& summary) -> bool override {
        const std::lock_guard lock(mtx_);
        last_ = summary;
        return true;
    }
    auto Load() -> std::optional<LastSessionSummary> override { return preloaded; }

    std::optional<LastSessionSummary> preloaded;

   private:
    std::mutex mtx_;
    std::optional<LastSessionSummary> last_;
};

auto MakeConfig() -> SessionConfig {
    SessionConfig cfg;
    cfg.match_uuid = "match-42";
    cfg.team_a_id = "home";
    cfg.team_b_id = "away";
    cfg.period_length_seconds = kPeriodLengthSeconds;
    cfg.video_output_path = "";  // skip dir creation
    cfg.thumbnail_output_path = "";
    return cfg;
}

auto SnapshotCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("snap-1");
    cmd.mutable_get_session_snapshot();
    return cmd;
}

// Handler + its injectable sources. Activity flags are plain bools the test
// flips; health providers stay unset (the U3 seam) unless a test wires them.
struct Fixture {
    FakeCleanup cleanup;
    SessionManager manager;
    sst::decision::ManualCameraState active_camera;
    sst::streaming::PreviewLayoutState preview_layout;
    bool recording{false};
    bool streaming{false};
    bool raw_capturing{false};
    SessionSnapshotHandler handler;

    explicit Fixture(sst::session::ISessionSummaryStore* store = nullptr, SessionTiming timing = {})
        : manager(cleanup, store, timing),
          handler(manager, active_camera, preview_layout,
                  SessionSnapshotHandler::Providers{
                      .is_recording = [this] { return recording; },
                      .is_streaming = [this] { return streaming; },
                      .is_raw_capturing = [this] { return raw_capturing; },
                      .camera0_health = {},
                      .camera1_health = {}},
                  [] { return kEpochStamp; }) {}

    // Drive the axes to session=Recording through the ordered F1 flow.
    auto DriveToRecording() -> void {
        ASSERT_TRUE(manager.OnConnect());
        ASSERT_TRUE(manager.OnWifiReady());
        ASSERT_TRUE(manager.ApplySessionConfig(MakeConfig()));
        ASSERT_TRUE(manager.OnOverlayConfigured());
        ASSERT_TRUE(manager.OnRecordingStart());
        recording = true;
    }
};

// Happy path (AE1 read half): a snapshot taken mid-recording carries the
// session axis, the monotonic recording clock, the absolute match state
// (incl. the state-health fields 9-11), the activity-flag truth, and the
// wifi-group axis. Spec-style assertion list — see the suppression rationale.
TEST(SessionSnapshotHandlerTest,  // NOLINT(readability-function-cognitive-complexity)
     SnapshotDuringRecordingCarriesActualState) {
    Fixture fixture;
    fixture.DriveToRecording();
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.score_a = 2;
        match.score_b = 1;
        match.period = 2;
        match.clock_seconds = kMatchClockSeconds;
        match.clock_running = true;
    });

    const auto resp = fixture.handler.Handle(SnapshotCmd());

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kSessionSnapshot);
    const auto& snap = resp.session_snapshot();
    EXPECT_EQ(snap.session_phase(), sst_cam::SESSION_RECORDING);
    EXPECT_TRUE(snap.is_recording());
    EXPECT_FALSE(snap.is_streaming());
    EXPECT_FALSE(snap.is_raw_capturing());
    EXPECT_TRUE(snap.has_recording_elapsed_seconds());
    EXPECT_TRUE(snap.wifi_group_up());
    // Absolute match state, incl. the unclamped monotonic clock (9), the
    // running bit (10), and the session's match uuid (11).
    ASSERT_TRUE(snap.has_match_state());
    EXPECT_EQ(snap.match_state().score_a(), 2U);
    EXPECT_EQ(snap.match_state().score_b(), 1U);
    EXPECT_EQ(snap.match_state().current_period(), 2U);
    ASSERT_TRUE(snap.match_state().has_elapsed_seconds());
    EXPECT_EQ(snap.match_state().elapsed_seconds(), kMatchClockSeconds);
    EXPECT_TRUE(snap.match_state().has_clock_running());
    EXPECT_TRUE(snap.match_state().clock_running());
    ASSERT_TRUE(snap.match_state().has_match_uuid());
    EXPECT_EQ(snap.match_state().match_uuid(), "match-42");
    EXPECT_EQ(snap.match_state().updated_at(), kEpochStamp);
    // elapsed_seconds is deliberately NOT clamped at period length (1234 > 600),
    // unlike time_remaining_s which is documented lossy.
    EXPECT_EQ(snap.match_state().time_remaining_s(), 0U);
    // Health is unreported until the health monitor (U3) is wired — absent,
    // never a fabricated OK.
    EXPECT_FALSE(snap.has_camera0_health());
    EXPECT_FALSE(snap.has_camera1_health());
    // No previous session while one is running.
    EXPECT_FALSE(snap.has_last_session());
}

// AE4 read half: after the auto-stop safety net ends an unsupervised recording,
// an idle snapshot carries the last-session summary with the auto-stop reason.
TEST(SessionSnapshotHandlerTest, SnapshotIdleAfterAutoStopCarriesSummary) {
    Fixture fixture(nullptr,
                    SessionTiming{.default_auto_stop = kAutoStop, .config_minute = kConfigMinute});
    fixture.DriveToRecording();
    fixture.manager.OnDisconnect();
    ASSERT_TRUE(WaitFor(
        [&fixture] { return fixture.manager.Phase() == sst::session::SessionPhase::kIdle; }));
    fixture.recording = false;

    const auto resp = fixture.handler.Handle(SnapshotCmd());

    const auto& snap = resp.session_snapshot();
    EXPECT_EQ(snap.session_phase(), sst_cam::SESSION_IDLE);
    ASSERT_TRUE(snap.has_last_session());
    EXPECT_EQ(snap.last_session().match_uuid(), "match-42");
    EXPECT_EQ(snap.last_session().end_reason(), sst_cam::SESSION_END_AUTO_STOP);
    EXPECT_TRUE(snap.last_session().has_file_valid());
    EXPECT_TRUE(snap.last_session().file_valid());
    EXPECT_FALSE(snap.is_recording());
}

// Edge: a first-ever connect (no prior session) reports the summary as ABSENT
// (has_ false) — never a zero-filled struct the app might mistake for history.
// The match state is equally absent: nothing was ever configured.
TEST(SessionSnapshotHandlerTest, NoPriorSessionReportsSummaryAbsent) {
    Fixture fixture;

    const auto resp = fixture.handler.Handle(SnapshotCmd());

    const auto& snap = resp.session_snapshot();
    EXPECT_EQ(snap.session_phase(), sst_cam::SESSION_IDLE);
    EXPECT_FALSE(snap.has_last_session());
    EXPECT_FALSE(snap.has_match_state());
    EXPECT_FALSE(snap.has_recording_elapsed_seconds());
    EXPECT_FALSE(snap.wifi_group_up());
}

// AE3: a manual disconnect→reconnect leaves the firmware's actual selections in
// the snapshot — the app adopts camera 1 / side-by-side, not defaults (the old
// reset-on-disconnect behavior is retired; selections reset at session END).
TEST(SessionSnapshotHandlerTest, ReconnectSnapshotReflectsActualSelections) {
    Fixture fixture;
    fixture.DriveToRecording();
    fixture.active_camera.Set(1);
    fixture.preview_layout.Set(sst::streaming::PreviewLayout::kSideBySide);

    fixture.manager.OnDisconnect();
    ASSERT_TRUE(fixture.manager.OnConnect());
    const auto resp = fixture.handler.Handle(SnapshotCmd());

    const auto& snap = resp.session_snapshot();
    EXPECT_EQ(snap.session_phase(), sst_cam::SESSION_RECORDING);
    ASSERT_TRUE(snap.has_active_camera_index());
    EXPECT_EQ(snap.active_camera_index(), 1U);
    ASSERT_TRUE(snap.has_preview_layout());
    EXPECT_EQ(snap.preview_layout(), sst_cam::PREVIEW_LAYOUT_SIDE_BY_SIDE);
    EXPECT_TRUE(snap.wifi_group_up());
}

// AE2: after a firmware reboot the snapshot has the exact shape of a
// first-ever connect (idle, no carryover) — plus the boot-reconciled summary
// when the reboot orphaned a session (loaded from the summary store).
TEST(SessionSnapshotHandlerTest, PostRebootSnapshotShapeMatchesFirstConnect) {
    FakeStore store;
    store.preloaded = LastSessionSummary{.match_uuid = "match-42",
                                         .end_reason = SessionEndReason::kReboot,
                                         .end_clock_seconds = kRebootEndClock,
                                         .file_valid = false};
    Fixture rebooted(&store);
    Fixture first_connect;

    const auto rebooted_snap = rebooted.handler.Handle(SnapshotCmd()).session_snapshot();
    const auto first_snap = first_connect.handler.Handle(SnapshotCmd()).session_snapshot();

    // Identical shape: same phase, same absences — no phantom session state.
    EXPECT_EQ(rebooted_snap.session_phase(), first_snap.session_phase());
    EXPECT_EQ(rebooted_snap.has_match_state(), first_snap.has_match_state());
    EXPECT_EQ(rebooted_snap.has_recording_elapsed_seconds(),
              first_snap.has_recording_elapsed_seconds());
    EXPECT_EQ(rebooted_snap.is_recording(), first_snap.is_recording());
    EXPECT_EQ(rebooted_snap.wifi_group_up(), first_snap.wifi_group_up());
    // The one difference: the orphaned session's reconciled summary.
    ASSERT_TRUE(rebooted_snap.has_last_session());
    EXPECT_EQ(rebooted_snap.last_session().end_reason(), sst_cam::SESSION_END_REBOOT);
    EXPECT_FALSE(rebooted_snap.last_session().file_valid());
    EXPECT_FALSE(first_snap.has_last_session());
}

// The U3 seam: a wired health provider surfaces its reading; a nullopt reading
// stays absent (unreported), asymmetrically per camera.
TEST(SessionSnapshotHandlerTest, HealthProviderSeamSurfacesReadings) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup);
    sst::decision::ManualCameraState camera;
    sst::streaming::PreviewLayoutState layout;
    SessionSnapshotHandler handler(
        manager, camera, layout,
        SessionSnapshotHandler::Providers{
            .is_recording = {},
            .is_streaming = {},
            .is_raw_capturing = {},
            .camera0_health = [] { return std::optional{sst_cam::CAMERA_HEALTH_RECOVERING}; },
            .camera1_health = [] { return std::optional<sst_cam::CameraHealth>{}; }},
        [] { return kEpochStamp; });

    const auto resp = handler.Handle(SnapshotCmd());
    const auto& snap = resp.session_snapshot();

    ASSERT_TRUE(snap.has_camera0_health());
    EXPECT_EQ(snap.camera0_health(), sst_cam::CAMERA_HEALTH_RECOVERING);
    EXPECT_FALSE(snap.has_camera1_health());
}

// ── Handshake integration (proto §9b order) ────────────────────────────────

class FakeStats final : public sst::control::ISystemStats {
   public:
    [[nodiscard]] auto Read() const -> sst::control::SystemStats override { return {}; }
};

// Integration: the canonical connect handshake — protocol gate → time push →
// snapshot — works end-to-end through the dispatcher against the registered
// handlers, in order, on one dispatcher.
TEST(SessionSnapshotHandlerTest, HandshakeOrderWorksThroughDispatcher) {
    auto fixture = std::make_shared<Fixture>();
    FakeStats stats;
    std::uint64_t applied_epoch = 0;

    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(std::make_shared<sst::control::DeviceHandler>(
        sst::config::DeviceData{}, stats, sst::control::DeviceHandler::Providers{}));
    dispatcher.Register(std::make_shared<sst::control::SetDeviceTimeHandler>(
        [&applied_epoch](std::uint64_t epoch_ms) {
            applied_epoch = epoch_ms;
            return true;
        },
        [] { return false; }, [] {}));
    dispatcher.Register(std::shared_ptr<SessionSnapshotHandler>(fixture, &fixture->handler));

    // 1. Protocol gate: the app requires >= 4 for this handshake.
    sst_cam::Command info;
    info.set_correlation_id("h-1");
    info.mutable_get_device_info();
    const auto info_resp = dispatcher.Dispatch(info);
    EXPECT_EQ(info_resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_GE(info_resp.device_info().protocol_version(), 4U);

    // 2. Wall-clock push.
    sst_cam::Command time_cmd;
    time_cmd.set_correlation_id("h-2");
    time_cmd.mutable_set_device_time()->set_epoch_ms(kEpochStamp);
    const auto time_resp = dispatcher.Dispatch(time_cmd);
    EXPECT_EQ(time_resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(applied_epoch, kEpochStamp);

    // 3. Snapshot read.
    const auto snap_resp = dispatcher.Dispatch(SnapshotCmd());
    EXPECT_EQ(snap_resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(snap_resp.correlation_id(), "snap-1");
    ASSERT_EQ(snap_resp.payload_case(), sst_cam::CommandResponse::kSessionSnapshot);
    EXPECT_EQ(snap_resp.session_snapshot().session_phase(), sst_cam::SESSION_IDLE);
}

}  // namespace
