// GetMatchState handler: live-match snapshot -> MatchState response (U6, R6).
// Pure — real SessionManager with a fake cleanup; no transport, no hardware.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/match-state.handler.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/session/models/session-config.hpp"

namespace {

using sst::control::MatchStateHandler;

// Default period length (seconds) for a session driven up to Configured.
constexpr std::int32_t kPeriodLengthSeconds = 600;
// Clock reading used by the populated-snapshot happy path (510s remain).
constexpr std::int32_t kElapsedSeconds = 90;
// Past-the-period clock reading used by the clamp test.
constexpr std::int32_t kOverrunSeconds = 650;
// Fixed "now" timestamp injected into the populated-snapshot test.
constexpr std::uint64_t kUpdatedAtStamp = 12345;

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override { return false; }
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}
};

// A session driven up to Configured with the given period length, ready to take
// match events.
struct Fixture {
    FakeCleanup cleanup;
    sst::session::SessionManager manager{cleanup};

    explicit Fixture(std::int32_t period_length = kPeriodLengthSeconds) {
        sst::session::SessionConfig cfg;
        cfg.team_a_id = "home";
        cfg.team_b_id = "away";
        cfg.period_length_seconds = period_length;
        cfg.video_output_path = "";
        cfg.thumbnail_output_path = "";
        manager.OnConnect();
        manager.OnWifiReady();
        manager.ApplySessionConfig(cfg);
        manager.OnOverlayConfigured();
    }
};

auto GetMatchStateCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("ms-1");
    cmd.mutable_get_match_state();
    return cmd;
}

// Asserts the score/period/team fields of a populated MatchState payload.
void ExpectSnapshotScores(const sst_cam::MatchState& state) {
    EXPECT_EQ(state.status(), sst_cam::MATCH_ACTIVE);
    EXPECT_EQ(state.current_period(), 1U);
    EXPECT_EQ(state.score_a(), 2U);
    EXPECT_EQ(state.score_b(), 1U);
    EXPECT_EQ(state.team_a_id(), "home");
    EXPECT_EQ(state.team_b_id(), "away");
}

// Asserts the derived timing fields of a populated MatchState payload. Split
// from the score asserts so neither helper trips the cognitive-complexity cap
// (gtest EXPECT_EQ macros each count as a branch).
void ExpectSnapshotTiming(const sst_cam::MatchState& state) {
    EXPECT_EQ(state.time_remaining_s(), 510U);  // 600 - 90
    EXPECT_EQ(state.updated_at(), kUpdatedAtStamp);
}

// Happy path: a populated live match maps onto a MatchState payload (OK status,
// echoed correlation_id, scores/period/teams from the snapshot).
TEST(MatchStateHandlerTest, ReportsLiveMatchSnapshot) {
    Fixture fixture(/*period_length=*/kPeriodLengthSeconds);
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.score_a = 2;
        match.score_b = 1;
        match.period = 1;
        match.clock_seconds = kElapsedSeconds;
        match.clock_running = true;
        match.segment = sst::session::MatchSegment::kInPlay;
    });

    MatchStateHandler handler(fixture.manager, [] { return kUpdatedAtStamp; });
    auto resp = handler.Handle(GetMatchStateCmd());

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kMatchState);
    ExpectSnapshotScores(resp.match_state());
    ExpectSnapshotTiming(resp.match_state());
}

// A paused clock reports MATCH_PAUSED; a full whistle reports MATCH_FINISHED.
TEST(MatchStateHandlerTest, StatusReflectsClockAndSegment) {
    Fixture fixture;
    MatchStateHandler handler(fixture.manager, [] { return std::uint64_t{0}; });

    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.period = 1;
        match.clock_running = false;
        match.segment = sst::session::MatchSegment::kInPlay;
    });
    EXPECT_EQ(handler.Handle(GetMatchStateCmd()).match_state().status(), sst_cam::MATCH_PAUSED);

    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.segment = sst::session::MatchSegment::kFullTime;
    });
    EXPECT_EQ(handler.Handle(GetMatchStateCmd()).match_state().status(), sst_cam::MATCH_FINISHED);
}

// Half-time segment maps onto MATCH_HALF_TIME regardless of the clock state.
TEST(MatchStateHandlerTest, HalfTimeSegmentReportsHalfTime) {
    Fixture fixture;
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.period = 1;
        match.clock_running = false;
        match.segment = sst::session::MatchSegment::kHalfTime;
    });
    MatchStateHandler handler(fixture.manager, [] { return std::uint64_t{0}; });
    EXPECT_EQ(handler.Handle(GetMatchStateCmd()).match_state().status(), sst_cam::MATCH_HALF_TIME);
}

// In-play but before kickoff (period == 0) is not started yet — even though a
// session is configured, no period has begun.
TEST(MatchStateHandlerTest, InPlayBeforeKickoffReportsNotStarted) {
    Fixture fixture;
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.period = 0;  // no period started
        match.clock_running = false;
        match.segment = sst::session::MatchSegment::kInPlay;
    });
    MatchStateHandler handler(fixture.manager, [] { return std::uint64_t{0}; });
    EXPECT_EQ(handler.Handle(GetMatchStateCmd()).match_state().status(),
              sst_cam::MATCH_NOT_STARTED);
}

// time_remaining_s clamps at zero when the locally-ticked clock has run past the
// configured period length (never wraps/underflows).
TEST(MatchStateHandlerTest, TimeRemainingClampsAtZeroWhenElapsedExceedsLength) {
    Fixture fixture(/*period_length=*/kPeriodLengthSeconds);
    fixture.manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.period = 1;
        match.clock_seconds = kOverrunSeconds;  // past the 600s period length
        match.clock_running = true;
        match.segment = sst::session::MatchSegment::kInPlay;
    });
    MatchStateHandler handler(fixture.manager, [] { return std::uint64_t{0}; });
    EXPECT_EQ(handler.Handle(GetMatchStateCmd()).match_state().time_remaining_s(), 0U);
}

// No active session (Idle) -> MATCH_NOT_STARTED, still OK, no crash.
TEST(MatchStateHandlerTest, NoSessionReportsNotStarted) {
    FakeCleanup cleanup;
    sst::session::SessionManager manager{cleanup};  // never configured -> Idle
    MatchStateHandler handler(manager, [] { return std::uint64_t{0}; });

    auto resp = handler.Handle(GetMatchStateCmd());
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.match_state().status(), sst_cam::MATCH_NOT_STARTED);
    EXPECT_EQ(resp.match_state().time_remaining_s(), 0U);
}

// Dispatcher parity: get_match_state no longer falls through to UNSUPPORTED once
// the handler is registered.
TEST(MatchStateHandlerTest, DispatcherRoutesGetMatchState) {
    Fixture fixture;
    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(
        std::make_shared<MatchStateHandler>(fixture.manager, [] { return std::uint64_t{0}; }));

    auto resp = dispatcher.Dispatch(GetMatchStateCmd());
    EXPECT_NE(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "ms-1");
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::kMatchState);
}

}  // namespace
