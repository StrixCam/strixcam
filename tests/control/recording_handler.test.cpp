// Recording handler: maps RecordingControl actions onto the recorder, gated by
// the session lifecycle (U10/U6). Pure — fake IRecordingService + real
// SessionManager driven through a fake ISessionCleanup.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "app/control/services/handlers/recording.handler.hpp"
#include "app/overlay/ports/overlay-timeline-recorder.hpp"
#include "app/raw_capture/ports/raw-capture-sink.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "app/storage/ports/recording-service.hpp"
#include "bluetooth.pb.h"
#include "domain/common/models/video-quality.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/storage/models/recording-state.hpp"

namespace {

using sst::control::RecordingHandler;
using sst::session::SessionConfig;
using sst::session::SessionManager;
using sst::session::SessionPhase;

class FakeRecorder final : public sst::storage::IRecordingService {
   public:
    auto StartRecording(const std::string& video, const std::string& thumb,
                        const sst::common::VideoQuality& quality) -> bool override {
        ++start_calls;
        last_video = video;
        last_thumb = thumb;
        last_quality = quality;
        return start_ok;
    }
    auto Pause() -> bool override {
        ++pause_calls;
        return pause_ok;
    }
    auto Resume() -> bool override {
        ++resume_calls;
        return resume_ok;
    }
    auto Stop() -> sst::storage::StopRecordingResult override {
        ++stop_calls;
        sst::storage::StopRecordingResult result;
        result.success = stop_ok;
        return result;
    }
    [[nodiscard]] auto CurrentState() const -> sst::storage::RecordingState override {
        return state;
    }

    int start_calls{0};
    int pause_calls{0};
    int resume_calls{0};
    int stop_calls{0};
    bool start_ok{true};
    bool pause_ok{true};
    bool resume_ok{true};
    bool stop_ok{true};
    std::string last_video;
    std::string last_thumb;
    sst::common::VideoQuality last_quality;
    sst::storage::RecordingState state{};
};

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> void override {}
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}
};

// No-op overlay timeline — these tests cover recording lifecycle, not timeline
// persistence (that has its own test). Just counts start/stop so a test could
// assert the wiring if needed.
class FakeTimeline final : public sst::overlay::IOverlayTimelineRecorder {
   public:
    auto Start(const std::filesystem::path& /*dir*/, std::uint64_t /*anchor_ms*/) -> void override {
        ++start_calls;
    }
    auto OnScene(std::uint64_t /*at_ms*/,
                 const sst::overlay::RenderScene& /*scene*/) -> void override {}
    auto Stop() -> void override { ++stop_calls; }

    int start_calls{0};
    int stop_calls{0};
};

// Zero clock — timeline anchor is irrelevant to recording-lifecycle assertions.
// Fake training-proxy sink: counts the handler's Start/Stop calls and can be set
// to fail Start (to prove proxy failure is non-fatal to the match record, U5).
class FakeProxy final : public sst::raw_capture::IRawCaptureSink {
   public:
    auto Start(const std::string& capture_group_id,
               const std::filesystem::path& output_dir) -> bool override {
        ++starts;
        last_group = capture_group_id;
        last_dir = output_dir;
        capturing_ = start_ok;
        return start_ok;
    }
    auto PushCamera(std::uint32_t /*camera_index*/,
                    const sst::capture::Frame& /*frame*/) -> void override {}
    auto Stop() -> bool override {
        ++stops;
        const bool was = capturing_;
        capturing_ = false;
        return was;
    }
    [[nodiscard]] auto IsCapturing() const -> bool override { return capturing_; }

    bool start_ok{true};
    int starts{0};
    int stops{0};
    std::string last_group;
    std::filesystem::path last_dir;

   private:
    bool capturing_{false};
};

auto ZeroClock() -> std::uint64_t { return 0; }

// Empty output paths keep the test off the filesystem (PrepareOutputDirs is a
// no-op for empty paths).
auto EmptyPathConfig() -> SessionConfig {
    SessionConfig cfg;
    cfg.match_uuid = "m";
    cfg.user_uuid = "u";
    return cfg;
}

auto AdvanceToConfigured(SessionManager& session_mgr) -> void {
    ASSERT_TRUE(session_mgr.OnConnect());
    ASSERT_TRUE(session_mgr.OnWifiReady());
    ASSERT_TRUE(session_mgr.ApplySessionConfig(EmptyPathConfig()));
}

auto AdvanceToReady(SessionManager& session_mgr) -> void {
    AdvanceToConfigured(session_mgr);
    ASSERT_TRUE(session_mgr.OnOverlayConfigured());
}

auto Cmd(sst_cam::RecordingAction action) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("r");
    cmd.mutable_recording_control()->set_action(action);
    return cmd;
}

auto StartCmdWithQuality(const sst::common::VideoQuality& mode) -> sst_cam::Command {
    auto cmd = Cmd(sst_cam::RECORDING_START);
    auto* quality = cmd.mutable_recording_control()->mutable_quality();
    quality->set_width(static_cast<std::uint32_t>(mode.width));
    quality->set_height(static_cast<std::uint32_t>(mode.height));
    quality->set_fps(static_cast<std::uint32_t>(mode.fps));
    return cmd;
}

auto StartCmdWithGroup(const std::string& group_id) -> sst_cam::Command {
    auto cmd = Cmd(sst_cam::RECORDING_START);
    cmd.mutable_recording_control()->set_capture_group_id(group_id);
    return cmd;
}

TEST(RecordingHandlerTest, StartInReadyPhaseStartsRecorderAndTransitions) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_START));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(recorder.start_calls, 1);
    EXPECT_EQ(session_mgr.Phase(), SessionPhase::kRecording);
}

// The gate (#16): a START before Ready must NOT touch the recorder — no stray
// MP4/thumbnail spun up only to be rolled back.
TEST(RecordingHandlerTest, StartBeforeReadyRejectedWithoutTouchingRecorder) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToConfigured(session_mgr);  // config present, but phase != Ready

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_START));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(recorder.start_calls, 0);
    EXPECT_EQ(recorder.stop_calls, 0);
    EXPECT_EQ(session_mgr.Phase(), SessionPhase::kConfigured);
}

// U8/R15: a supported record quality on the START command reaches the recording
// service verbatim, independent of any stream quality.
TEST(RecordingHandlerTest, SupportedRecordQualityForwarded) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    const sst::common::VideoQuality kMode{1920, 1080, 30};  // advertised mode
    auto resp = handler.Handle(StartCmdWithQuality(kMode));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(recorder.last_quality, kMode);
}

// U8/R16: an unsupported requested mode is rejected to the firmware default
// (unset) rather than driving the pipeline to an unadvertised resolution.
TEST(RecordingHandlerTest, UnsupportedRecordQualityFallsBackToDefault) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    const sst::common::VideoQuality k4K{3840, 2160, 30};  // not advertised
    auto resp = handler.Handle(StartCmdWithQuality(k4K));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_FALSE(recorder.last_quality.IsSet());
}

// A START with no quality field leaves the recorder on its default (unset).
TEST(RecordingHandlerTest, MissingRecordQualityIsUnset) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_START));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_FALSE(recorder.last_quality.IsSet());
}

TEST(RecordingHandlerTest, StartWithNoSessionConfigErrors) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    ASSERT_TRUE(session_mgr.OnConnect());
    ASSERT_TRUE(session_mgr.OnWifiReady());  // WifiReady, no config yet

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_START));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(recorder.start_calls, 0);
}

TEST(RecordingHandlerTest, StartRolledBackWhenRecorderStartsButSmRejects) {
    // Recorder accepts the start, but force the SM to reject by being in a
    // non-Ready phase is already covered above; here the recorder itself fails.
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    recorder.start_ok = false;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_START));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(recorder.start_calls, 1);
    EXPECT_EQ(session_mgr.Phase(), SessionPhase::kReady);  // no transition on failed start
}

TEST(RecordingHandlerTest, StopStopsRecorderAndReturnsToReady) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);
    ASSERT_EQ(handler.Handle(Cmd(sst_cam::RECORDING_START)).status(), sst_cam::ResponseStatus::OK);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_STOP));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(recorder.stop_calls, 1);
    EXPECT_EQ(session_mgr.Phase(), SessionPhase::kReady);
}

TEST(RecordingHandlerTest, PauseWhileNotRecordingErrors) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    recorder.pause_ok = false;  // recorder reports it isn't recording
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_PAUSE));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
}

// U5 / AE1: a record START carrying a capture_group_id couples the training proxy
// to the match — it starts alongside the recorder with that group id.
TEST(RecordingHandlerTest, StartWithGroupIdCouplesProxy) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(StartCmdWithGroup("grp-42"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(recorder.start_calls, 1);
    EXPECT_EQ(proxy.starts, 1);
    EXPECT_EQ(proxy.last_group, "grp-42");
    EXPECT_TRUE(proxy.IsCapturing());
}

// A record START with NO capture_group_id records normally but does not start a
// proxy (backward-compatible with pre-coupling apps).
TEST(RecordingHandlerTest, StartWithoutGroupIdDoesNotStartProxy) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    ASSERT_EQ(handler.Handle(Cmd(sst_cam::RECORDING_START)).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(recorder.start_calls, 1);
    EXPECT_EQ(proxy.starts, 0);
}

// STOP stops the coupled proxy alongside the recorder.
TEST(RecordingHandlerTest, StopStopsCoupledProxy) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);
    ASSERT_EQ(handler.Handle(StartCmdWithGroup("g")).status(), sst_cam::ResponseStatus::OK);

    auto resp = handler.Handle(Cmd(sst_cam::RECORDING_STOP));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_GE(proxy.stops, 1);
    EXPECT_FALSE(proxy.IsCapturing());
}

// Proxy-start failure is NON-FATAL: the match record still starts (training
// footage is best-effort).
TEST(RecordingHandlerTest, ProxyStartFailureDoesNotFailRecord) {
    FakeCleanup cleanup;
    SessionManager session_mgr(cleanup);
    FakeRecorder recorder;
    FakeTimeline timeline;
    FakeProxy proxy;
    proxy.start_ok = false;  // proxy pipeline refuses to start
    RecordingHandler handler(session_mgr, recorder, timeline, proxy, ZeroClock);
    AdvanceToReady(session_mgr);

    auto resp = handler.Handle(StartCmdWithGroup("g"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);  // record unaffected
    EXPECT_EQ(recorder.start_calls, 1);
    EXPECT_EQ(session_mgr.Phase(), SessionPhase::kRecording);
}

}  // namespace
