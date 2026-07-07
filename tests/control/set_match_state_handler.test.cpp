// SetMatchState handler: absolute live-match overwrite for post-disconnect
// reconciliation (state-health cycle U2, proto §9b). Pure — real SessionManager
// + OverlayController with fake renderer/sink/cleanup; no transport.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/match-state.handler.hpp"
#include "app/control/services/handlers/set-match-state.handler.hpp"
#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/session/models/session-config.hpp"

namespace {

using sst::control::MatchStateHandler;
using sst::control::SetMatchStateHandler;
using sst::overlay::IOverlayRenderer;
using sst::overlay::IOverlaySink;
using sst::overlay::OverlayController;
using sst::overlay::RenderScene;
using sst::overlay::RgbaImage;

// Overlay canvas size for the test controller; any small square works.
constexpr std::uint32_t kOverlayDim = 64;
// Reconciled clock value pushed by the overwrite tests — deliberately past the
// configured period length to prove elapsed_seconds is never clamped.
constexpr std::uint32_t kReconciledClock = 700;
// Configured period length for the fixtures (seconds).
constexpr std::int32_t kPeriodLength = 600;
// Pre-set clock value the partial-set test expects to survive untouched.
constexpr std::uint32_t kPresetClock = 500;
// The one field the partial-set test writes.
constexpr std::uint32_t kPartialScore = 5;

class FakeRenderer final : public IOverlayRenderer {
   public:
    // Param order fixed by the IOverlayRenderer::Render interface contract.
    auto Render(const RenderScene& /*scene*/,
                std::uint32_t out_width,  // NOLINT(bugprone-easily-swappable-parameters)
                                          // floor-ok: test double; order fixed by
                                          // IOverlayRenderer::Render, cannot reorder in override
                std::uint32_t out_height) -> RgbaImage override {
        RgbaImage img;
        img.width = out_width;
        img.height = out_height;
        img.stride = out_width * 4;
        img.pixels.assign(static_cast<std::size_t>(img.stride) * out_height, 0);
        return img;
    }
};

class FakeSink final : public IOverlaySink {
   public:
    auto PushFrame(const RgbaImage& /*frame*/) -> void override { ++pushes; }
    auto Clear() -> void override { ++clears; }
    int pushes{0};
    int clears{0};
};

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override { return false; }
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}
};

struct Fixture {
    FakeCleanup cleanup;
    FakeRenderer renderer;
    FakeSink sink;
    sst::session::SessionManager manager{cleanup};
    OverlayController controller{renderer, sink, sst::common::OutputSize{kOverlayDim, kOverlayDim}};
    SetMatchStateHandler handler{manager, controller, [] { return std::uint64_t{0}; }};

    Fixture() {
        sst::session::SessionConfig cfg;
        cfg.match_uuid = "match-42";
        cfg.team_a_id = "home";
        cfg.team_b_id = "away";
        cfg.period_length_seconds = kPeriodLength;
        cfg.video_output_path = "";  // skip dir creation
        cfg.thumbnail_output_path = "";
        manager.OnConnect();
        manager.OnWifiReady();
        manager.ApplySessionConfig(cfg);
        manager.OnOverlayConfigured();
    }
};

auto SetMatchStateCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("sms-1");
    cmd.mutable_set_match_state();
    return cmd;
}

// Happy path: a full overwrite lands every field, and a subsequent
// GetMatchState (the other reader of the same live match) agrees — including
// the unclamped elapsed_seconds (9), clock_running (10), and match_uuid (11).
TEST(SetMatchStateHandlerTest,  // NOLINT(readability-function-cognitive-complexity)
     FullOverwriteThenGetMatchStateAgrees) {
    Fixture fixture;
    auto cmd = SetMatchStateCmd();
    auto* msg = cmd.mutable_set_match_state();
    msg->set_score_a(3);
    msg->set_score_b(2);
    msg->set_current_period(2);
    msg->set_elapsed_seconds(kReconciledClock);
    msg->set_clock_running(true);
    msg->set_status(sst_cam::MATCH_ACTIVE);

    const auto set_resp = fixture.handler.Handle(cmd);
    EXPECT_EQ(set_resp.status(), sst_cam::ResponseStatus::OK);

    MatchStateHandler reader(fixture.manager, [] { return std::uint64_t{0}; });
    sst_cam::Command get;
    get.mutable_get_match_state();
    const auto get_resp = reader.Handle(get);
    const auto& state = get_resp.match_state();
    EXPECT_EQ(state.score_a(), 3U);
    EXPECT_EQ(state.score_b(), 2U);
    EXPECT_EQ(state.current_period(), 2U);
    EXPECT_EQ(state.status(), sst_cam::MATCH_ACTIVE);
    ASSERT_TRUE(state.has_elapsed_seconds());
    EXPECT_EQ(state.elapsed_seconds(), kReconciledClock);  // 700 > 600: not clamped
    EXPECT_EQ(state.time_remaining_s(), 0U);               // the clamped view bottoms out
    EXPECT_TRUE(state.clock_running());
    EXPECT_EQ(state.match_uuid(), "match-42");
}

// Edge: a partial set writes ONLY the fields present — everything absent keeps
// the value the firmware carried through the disconnect.
TEST(SetMatchStateHandlerTest, PartialSetLeavesAbsentFieldsUntouched) {
    Fixture fixture;
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.score_a = 1;
        match.score_b = 1;
        match.period = 1;
        match.clock_seconds = kPresetClock;
        match.clock_running = true;
    });

    auto cmd = SetMatchStateCmd();
    cmd.mutable_set_match_state()->set_score_a(kPartialScore);
    EXPECT_EQ(fixture.handler.Handle(cmd).status(), sst_cam::ResponseStatus::OK);

    const auto snap = fixture.manager.Snapshot();
    EXPECT_EQ(snap.match.score_a, kPartialScore);       // written
    EXPECT_EQ(snap.match.score_b, 1U);                  // untouched
    EXPECT_EQ(snap.match.period, 1U);                   // untouched
    EXPECT_EQ(snap.match.clock_seconds, kPresetClock);  // untouched
    EXPECT_TRUE(snap.match.clock_running);              // untouched
}

// Terminal statuses map onto the display segment (the two segments that carry
// their own information; ACTIVE/PAUSED derive from clock_running instead).
TEST(SetMatchStateHandlerTest, TerminalStatusMapsToSegment) {
    Fixture fixture;
    auto cmd = SetMatchStateCmd();
    cmd.mutable_set_match_state()->set_status(sst_cam::MATCH_HALF_TIME);
    EXPECT_EQ(fixture.handler.Handle(cmd).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(fixture.manager.Snapshot().match.segment, sst::session::MatchSegment::kHalfTime);

    cmd.mutable_set_match_state()->set_status(sst_cam::MATCH_FINISHED);
    EXPECT_EQ(fixture.handler.Handle(cmd).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(fixture.manager.Snapshot().match.segment, sst::session::MatchSegment::kFullTime);
}

// A successful reconcile refreshes the overlay immediately — the on-frame
// scoreboard must not lag the app by a clock tick.
TEST(SetMatchStateHandlerTest, SuccessfulSetRefreshesOverlay) {
    Fixture fixture;
    const int pushes_before = fixture.sink.pushes;
    auto cmd = SetMatchStateCmd();
    cmd.mutable_set_match_state()->set_score_a(1);
    EXPECT_EQ(fixture.handler.Handle(cmd).status(), sst_cam::ResponseStatus::OK);
    EXPECT_GT(fixture.sink.pushes, pushes_before);
}

// Error: with no active session there is no live match to overwrite.
TEST(SetMatchStateHandlerTest, NoActiveSessionRejected) {
    FakeCleanup cleanup;
    FakeRenderer renderer;
    FakeSink sink;
    sst::session::SessionManager manager{cleanup};  // never configured -> Idle
    OverlayController controller{renderer, sink, sst::common::OutputSize{kOverlayDim, kOverlayDim}};
    SetMatchStateHandler handler{manager, controller, [] { return std::uint64_t{0}; }};

    auto cmd = SetMatchStateCmd();
    cmd.mutable_set_match_state()->set_score_a(1);
    const auto resp = handler.Handle(cmd);
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
}

// Dispatcher parity: set_match_state routes to the handler (correlation echoed).
TEST(SetMatchStateHandlerTest, DispatcherRoutesSetMatchState) {
    auto fixture = std::make_shared<Fixture>();
    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(std::shared_ptr<SetMatchStateHandler>(fixture, &fixture->handler));

    auto cmd = SetMatchStateCmd();
    cmd.mutable_set_match_state()->set_score_b(4);
    const auto resp = dispatcher.Dispatch(cmd);
    EXPECT_NE(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "sms-1");
    EXPECT_EQ(fixture->manager.Snapshot().match.score_b, 4U);
}

}  // namespace
