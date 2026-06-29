// Match event handler: score/clock/banner -> session + overlay (U9, R18/R20).
// Pure — real SessionManager + OverlayController with fake renderer/sink/cleanup.

#include <gtest/gtest.h>

#include <cstdint>

#include "app/control/services/handlers/match.handler.hpp"
#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/session/models/session-config.hpp"

namespace {

using sst::control::MatchHandler;
using sst::overlay::IOverlayRenderer;
using sst::overlay::IOverlaySink;
using sst::overlay::OverlayController;
using sst::overlay::RenderScene;
using sst::overlay::RgbaImage;

class FakeRenderer final : public IOverlayRenderer {
   public:
    // Param order fixed by the IOverlayRenderer::Render interface contract;
    // cannot reorder in an override.
    auto Render(const RenderScene& /*scene*/,
                std::uint32_t out_width,  // NOLINT(bugprone-easily-swappable-parameters) floor-ok:
                                          // test double; order fixed by IOverlayRenderer::Render,
                                          // cannot reorder in override
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
    auto FinalizeRecording() -> void override {}
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
};

// Overlay canvas size for the test controller; any small square works.
constexpr std::uint32_t kOverlayDim = 64;

struct Fixture {
    FakeCleanup cleanup;
    FakeRenderer renderer;
    FakeSink sink;
    sst::session::SessionManager sm{cleanup};
    OverlayController controller{renderer, sink, sst::common::OutputSize{kOverlayDim, kOverlayDim}};

    Fixture() {
        sst::session::SessionConfig cfg;
        cfg.team_a_id = "home";
        cfg.team_b_id = "away";
        cfg.team_a_name = "Home";
        cfg.team_b_name = "Away";
        cfg.video_output_path = "";  // skip dir creation
        cfg.thumbnail_output_path = "";
        sm.OnConnect();
        sm.OnWifiReady();
        sm.ApplySessionConfig(cfg);
        sm.OnOverlayConfigured();
    }
};

auto ScoreCmd(const std::string& team, std::int32_t delta) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("s");
    auto* score = cmd.mutable_score_update();
    score->set_team_id(team);
    score->set_delta(delta);
    return cmd;
}

auto MatchCtl(sst_cam::MatchControlAction action, std::uint32_t period = 0) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("m");
    auto* match_ctl = cmd.mutable_match_control();
    match_ctl->set_action(action);
    match_ctl->set_period(period);
    return cmd;
}

// Final whistle clears the overlay: a finished match has no active scoreboard,
// so the live preview must not keep showing it.
TEST(MatchHandlerTest, FinalWhistleClearsOverlay) {
    Fixture fixture;
    MatchHandler handler(fixture.sm, fixture.controller, [] { return std::uint64_t{0}; });

    handler.Handle(MatchCtl(sst_cam::MATCH_KICKOFF, 1));
    EXPECT_EQ(fixture.sink.clears, 0);
    EXPECT_EQ(handler.Handle(MatchCtl(sst_cam::MATCH_FINAL_WHISTLE, 1)).status(),
              sst_cam::ResponseStatus::OK);
    EXPECT_EQ(fixture.sink.clears, 1);
}

// After the final whistle the clock is stopped, so a 1 Hz clock tick must NOT
// resurrect the overlay. Regression: TickClock keyed its refresh off
// ApplyMatchUpdate's always-true return, re-pushing the board one tick after the
// final-whistle clear (overlay flickered back ~1s after ending the match).
TEST(MatchHandlerTest, TickAfterFinalWhistleDoesNotResurrectOverlay) {
    Fixture fixture;
    MatchHandler handler(fixture.sm, fixture.controller, [] { return std::uint64_t{0}; });

    handler.Handle(MatchCtl(sst_cam::MATCH_KICKOFF, 1));
    handler.Handle(MatchCtl(sst_cam::MATCH_FINAL_WHISTLE, 1));
    const int pushes_after_whistle = fixture.sink.pushes;

    handler.TickClock();
    handler.TickClock();

    EXPECT_EQ(fixture.sink.pushes, pushes_after_whistle);  // clock stopped -> no re-push
}

// ScoreUpdate maps team_id -> home/away and updates live match.
TEST(MatchHandlerTest, ScoreUpdateUpdatesLiveMatch) {
    Fixture fixture;
    MatchHandler handler(fixture.sm, fixture.controller, [] { return std::uint64_t{0}; });

    EXPECT_EQ(handler.Handle(ScoreCmd("home", 1)).status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(fixture.sm.Snapshot().match.score_a, 1U);
    handler.Handle(ScoreCmd("away", 2));
    EXPECT_EQ(fixture.sm.Snapshot().match.score_b, 2U);

    // Negative delta clamps at zero (any value past the current score of 1).
    handler.Handle(ScoreCmd("home", -5));  // NOLINT(readability-magic-numbers)
    EXPECT_EQ(fixture.sm.Snapshot().match.score_a, 0U);
}

// Unknown team_id is an error.
TEST(MatchHandlerTest, ScoreUpdateUnknownTeamErrors) {
    Fixture fixture;
    MatchHandler handler(fixture.sm, fixture.controller, [] { return std::uint64_t{0}; });
    auto resp = handler.Handle(ScoreCmd("ghost", 1));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(resp.error_message().empty());
}

// R20: the local clock only advances while running; pause halts it, resume
// continues — the camera never advances the clock without an app event.
// A single linear clock-progression scenario; the cognitive-complexity score is
// inflated by gtest macro expansion and splitting the step-by-step narrative into
// helpers would obscure it — hence the suppression on the next line.
TEST(MatchHandlerTest,  // NOLINT(readability-function-cognitive-complexity)
     ClockIsDisplayOnlyAndGatedByAppEvents) {
    Fixture fixture;
    MatchHandler handler(fixture.sm, fixture.controller, [] { return std::uint64_t{0}; });

    // Before kickoff the clock is not running: ticks do nothing.
    handler.TickClock();
    EXPECT_EQ(fixture.sm.Snapshot().match.clock_seconds, 0U);

    handler.Handle(MatchCtl(sst_cam::MATCH_KICKOFF, 1));
    EXPECT_EQ(fixture.sm.Snapshot().match.period, 1U);
    handler.TickClock();
    handler.TickClock();
    EXPECT_EQ(fixture.sm.Snapshot().match.clock_seconds, 2U);

    // Pause halts the tick.
    handler.Handle(MatchCtl(sst_cam::MATCH_CLOCK_PAUSE));
    handler.TickClock();
    EXPECT_EQ(fixture.sm.Snapshot().match.clock_seconds, 2U);

    // Resume continues it.
    handler.Handle(MatchCtl(sst_cam::MATCH_CLOCK_RESUME));
    handler.TickClock();
    EXPECT_EQ(fixture.sm.Snapshot().match.clock_seconds, 3U);

    // Final whistle stops the clock and marks full-time.
    handler.Handle(MatchCtl(sst_cam::MATCH_FINAL_WHISTLE));
    handler.TickClock();
    EXPECT_EQ(fixture.sm.Snapshot().match.clock_seconds, 3U);
    EXPECT_EQ(fixture.sm.Snapshot().match.segment, sst::session::MatchSegment::kFullTime);
}

}  // namespace
