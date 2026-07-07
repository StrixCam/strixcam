// Session state model — sessions OUTLIVE connections (orthogonal axes:
// app_connected ⟂ wifi_group ⟂ session).
//
// Pure — no hardware. Drives the axes through connect/disconnect/wifi/config/
// record events, exercises the auto-stop safety net on scaled-down timing, the
// single finalize gate (CAS to Finalizing), and the last-session summary
// (in-memory + write-ahead persistence through a fake store).
//
// The pre-rework transition behavior (ordered ladder, §11 config gate,
// mid-recording config rejection, new-match score reset, cleanup fan-out,
// exception-safe fan-out) was characterized by the previous revision of this
// file; those invariants are re-derived below against the new axes.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/session/ports/session-cleanup.hpp"
#include "app/session/ports/session-summary-store.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/session/models/session-summary.hpp"

namespace fs = std::filesystem;

namespace {

using sst::session::LastSessionSummary;
using sst::session::SessionConfig;
using sst::session::SessionEndReason;
using sst::session::SessionManager;
using sst::session::SessionPhase;
using sst::session::SessionTiming;

// Scaled-down timing so auto-stop scenarios run in milliseconds. The margins
// are generous (reconnect at 1/4 of the window, fire wait ≤ 5 s) to stay
// stable under qemu in CI.
constexpr std::chrono::milliseconds kAutoStop{200};
constexpr std::chrono::milliseconds kConfigMinute{40};
constexpr std::chrono::milliseconds kBeforeTimeout{50};
constexpr std::chrono::milliseconds kSettleWait{600};
constexpr std::chrono::seconds kFireWait{5};
constexpr std::chrono::milliseconds kPollInterval{5};

auto TestTiming() -> SessionTiming {
    return SessionTiming{.default_auto_stop = kAutoStop, .config_minute = kConfigMinute};
}

// Poll `pred` until true or `timeout` elapses. Returns the final value.
auto WaitFor(const std::function<bool()>& pred,
             std::chrono::milliseconds timeout = kFireWait) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return pred();
}

// Counting session-end fan-out target; safe to invoke from the timer thread.
class FakeCleanup : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override {
        ++finalize_recording;
        return finalize_ok;
    }
    auto StopStreaming() -> void override { ++stop_streaming; }
    auto TeardownWifiDirect() -> void override { ++teardown_wifi; }
    auto ResetSelections() -> void override { ++reset_selections; }

    std::atomic<int> finalize_recording{0};
    std::atomic<int> stop_streaming{0};
    std::atomic<int> teardown_wifi{0};
    std::atomic<int> reset_selections{0};
    bool finalize_ok{true};
};

// In-memory store capturing every Persist() so tests can assert the write-ahead
// sequence (recording start → reboot/invalid; clean stop → reboot/valid;
// session end → real reason).
class FakeStore final : public sst::session::ISessionSummaryStore {
   public:
    auto Persist(const LastSessionSummary& summary) -> bool override {
        const std::lock_guard lock(mtx_);
        persisted_.push_back(summary);
        return true;
    }
    auto Load() -> std::optional<LastSessionSummary> override { return preloaded; }

    auto Persisted() -> std::vector<LastSessionSummary> {
        const std::lock_guard lock(mtx_);
        return persisted_;
    }

    std::optional<LastSessionSummary> preloaded;

   private:
    std::mutex mtx_;
    std::vector<LastSessionSummary> persisted_;
};

// Unique temp root per test to keep filesystem state isolated (no shared dirs).
auto MakeTempRoot() -> fs::path {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("sst_session_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
}

auto MakeConfig(const fs::path& root) -> SessionConfig {
    // A regulation soccer half is 45 minutes; period_length_seconds is in seconds.
    constexpr std::uint32_t kHalfLengthSeconds = 45U * 60U;
    SessionConfig config;
    config.match_uuid = "match-uuid";
    config.user_uuid = "user-uuid";
    config.sport = "SPORT_SOCCER";
    config.num_periods = 2;
    config.period_length_seconds = kHalfLengthSeconds;
    config.video_output_path = (root / "video/user-uuid/match-uuid/match-uuid.mp4").string();
    config.thumbnail_output_path =
        (root / "thumbnail/user-uuid/match-uuid/match-uuid.jpg").string();
    config.team_a_name = "Home";
    config.team_b_name = "Away";
    return config;
}

// Drive the axes through the full ordered F1 flow (connect → group → config →
// overlay), landing on session=Ready.
auto AdvanceToReady(SessionManager& manager, const SessionConfig& cfg) {
    ASSERT_TRUE(manager.OnConnect());
    ASSERT_TRUE(manager.OnWifiReady());
    ASSERT_TRUE(manager.ApplySessionConfig(cfg));
    ASSERT_TRUE(manager.OnOverlayConfigured());
}

// ── Re-derived ladder invariants ────────────────────────────────────────────

// Ordered commands drive the session axis Idle -> Configured -> Ready ->
// Recording, PushSessionConfig creates both output directories, and the
// connection/group axes track their own events.
TEST(SessionManagerTest, OrderedTransitionsAndDirCreation) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);
    EXPECT_FALSE(manager.Snapshot().app_connected);
    ASSERT_TRUE(manager.OnConnect());
    EXPECT_TRUE(manager.Snapshot().app_connected);
    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);  // connection is not a session
    ASSERT_TRUE(manager.OnWifiReady());
    EXPECT_TRUE(manager.Snapshot().wifi_group_up);
    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);
    ASSERT_TRUE(manager.ApplySessionConfig(cfg));
    EXPECT_EQ(manager.Phase(), SessionPhase::kConfigured);
    ASSERT_TRUE(manager.OnOverlayConfigured());
    EXPECT_EQ(manager.Phase(), SessionPhase::kReady);
    ASSERT_TRUE(manager.OnRecordingStart());
    EXPECT_EQ(manager.Phase(), SessionPhase::kRecording);

    EXPECT_TRUE(fs::exists(fs::path{cfg.video_output_path}.parent_path()));
    EXPECT_TRUE(fs::exists(fs::path{cfg.thumbnail_output_path}.parent_path()));

    fs::remove_all(root);
}

// Out-of-order: overlay/config/record before its prerequisite is rejected.
TEST(SessionManagerTest, OutOfOrderTransitionsRejected) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    // Overlay before any session config.
    EXPECT_FALSE(manager.OnOverlayConfigured());
    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);

    // Session config before the WiFi group is up (contract §11).
    ASSERT_TRUE(manager.OnConnect());
    EXPECT_FALSE(manager.ApplySessionConfig(cfg));
    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);

    // Recording before Ready.
    ASSERT_TRUE(manager.OnWifiReady());
    ASSERT_TRUE(manager.ApplySessionConfig(cfg));
    EXPECT_FALSE(manager.OnRecordingStart());  // still Configured, not Ready
    EXPECT_EQ(manager.Phase(), SessionPhase::kConfigured);

    fs::remove_all(root);
}

// A group formed with no app connected is rejected (it would leak unattended).
TEST(SessionManagerTest, WifiReadyRequiresConnectedApp) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    EXPECT_FALSE(manager.OnWifiReady());
    EXPECT_FALSE(manager.Snapshot().wifi_group_up);
}

// Re-pushing session config mid-recording is rejected; a re-push while
// Configured/Ready is accepted and keeps the session state.
TEST(SessionManagerTest, ApplySessionConfigRejectedWhileRecording) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    EXPECT_TRUE(manager.ApplySessionConfig(cfg));  // re-push while Ready: accepted
    EXPECT_EQ(manager.Phase(), SessionPhase::kReady);
    ASSERT_TRUE(manager.OnRecordingStart());

    EXPECT_FALSE(manager.ApplySessionConfig(cfg));
    EXPECT_EQ(manager.Phase(), SessionPhase::kRecording);

    fs::remove_all(root);
}

// #6 stale-overlay fix: a config for a NEW match resets the scoreboard to zero,
// while a same-match re-push preserves the score the app already pushed.
TEST(SessionManagerTest, NewMatchConfigResetsScoreSameMatchKeeps) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.score_a = 3;
        match.score_b = 2;
        match.period = 2;
    }));

    // Same match (same match_uuid) re-push: the live score survives.
    ASSERT_TRUE(manager.ApplySessionConfig(cfg));
    EXPECT_EQ(manager.Snapshot().match.score_a, 3U);
    EXPECT_EQ(manager.Snapshot().match.score_b, 2U);

    // A DIFFERENT match: scoreboard resets to zero.
    auto next = cfg;
    next.match_uuid = "match-uuid-2";
    next.video_output_path = (root / "video/user-uuid/match-uuid-2/match-uuid-2.mp4").string();
    next.thumbnail_output_path =
        (root / "thumbnail/user-uuid/match-uuid-2/match-uuid-2.jpg").string();
    ASSERT_TRUE(manager.ApplySessionConfig(next));

    const auto after = manager.Snapshot();
    EXPECT_EQ(after.match.score_a, 0U);
    EXPECT_EQ(after.match.score_b, 0U);
    ASSERT_TRUE(after.config.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(after.config->match_uuid, "match-uuid-2");

    fs::remove_all(root);
}

// ── Sessions outlive connections ────────────────────────────────────────────

// F2/AE1 firmware half: a disconnect during recording keeps the session and
// the WiFi group alive (no cleanup fan-out) and arms the auto-stop timer; a
// reconnect cancels it and finds the session unchanged — including the
// selections (ResetSelections is a session-END action now, never disconnect).
TEST(SessionManagerTest, DisconnectDuringRecordingKeepsSessionAndReconnectCancels) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    manager.OnDisconnect();
    auto during = manager.Snapshot();
    EXPECT_FALSE(during.app_connected);
    EXPECT_TRUE(during.wifi_group_up);
    EXPECT_EQ(during.phase, SessionPhase::kRecording);
    EXPECT_EQ(cleanup.finalize_recording.load(), 0);
    EXPECT_EQ(cleanup.reset_selections.load(), 0);

    // Reconnect before the timeout: accepted in ANY session state (AE3 shape).
    std::this_thread::sleep_for(kBeforeTimeout);
    EXPECT_TRUE(manager.OnConnect());

    // Wait well past the original deadline: the cancel won — nothing stopped.
    std::this_thread::sleep_for(kSettleWait);
    const auto after = manager.Snapshot();
    EXPECT_TRUE(after.app_connected);
    EXPECT_EQ(after.phase, SessionPhase::kRecording);
    EXPECT_TRUE(after.wifi_group_up);
    EXPECT_EQ(cleanup.finalize_recording.load(), 0);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 0);
    EXPECT_EQ(cleanup.reset_selections.load(), 0);
    EXPECT_FALSE(after.last_summary.has_value());

    fs::remove_all(root);
}

// AE4: with the app away, the auto-stop safety net fires after the configured
// window — recording finalized, summary recorded with reason=auto-stop, group
// torn down, session back to Idle.
TEST(SessionManagerTest, AutoStopFinalizesAndRecordsSummary) {
    FakeCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());
    manager.OnDisconnect();

    ASSERT_TRUE(WaitFor([&] { return manager.Phase() == SessionPhase::kIdle; }));
    const auto after = manager.Snapshot();
    EXPECT_EQ(cleanup.finalize_recording.load(), 1);
    EXPECT_EQ(cleanup.stop_streaming.load(), 1);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);
    EXPECT_EQ(cleanup.reset_selections.load(), 1);
    EXPECT_FALSE(after.wifi_group_up);
    EXPECT_FALSE(after.config.has_value());
    ASSERT_TRUE(after.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    const auto& summary = *after.last_summary;
    EXPECT_EQ(summary.match_uuid, "match-uuid");
    EXPECT_EQ(summary.end_reason, SessionEndReason::kAutoStop);
    EXPECT_TRUE(summary.file_valid);  // FinalizeRecording succeeded

    // The store's final record is the real summary, not the write-ahead.
    const auto persisted = store.Persisted();
    ASSERT_FALSE(persisted.empty());
    EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kAutoStop);

    fs::remove_all(root);
}

// Auto-stop covers a streaming-only session too (nothing recorded): the
// summary is still written, with file-valid=false.
TEST(SessionManagerTest, AutoStopEndsStreamingOnlySession) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);  // Ready, never recording
    manager.OnDisconnect();

    ASSERT_TRUE(WaitFor([&] { return manager.Phase() == SessionPhase::kIdle; }));
    const auto after = manager.Snapshot();
    EXPECT_EQ(cleanup.stop_streaming.load(), 1);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);
    ASSERT_TRUE(after.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_FALSE(after.last_summary->file_valid);

    fs::remove_all(root);
}

// The auto-stop window comes from the session config when the app set it
// (auto_stop_minutes × minute unit), not the firmware default.
TEST(SessionManagerTest, AutoStopWindowFromSessionConfig) {
    FakeCleanup cleanup;
    // Default so large it can never fire inside this test — proof the config
    // window (2 × 40 ms) is the one used.
    constexpr std::chrono::minutes kNeverInThisTest{30};
    SessionManager manager(
        cleanup, nullptr,
        SessionTiming{.default_auto_stop = kNeverInThisTest, .config_minute = kConfigMinute});
    const fs::path root = MakeTempRoot();
    auto cfg = MakeConfig(root);
    cfg.auto_stop_minutes = 2;

    AdvanceToReady(manager, cfg);
    manager.OnDisconnect();

    ASSERT_TRUE(WaitFor([&] { return manager.Phase() == SessionPhase::kIdle; }));
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);

    fs::remove_all(root);
}

// Edge: disconnect with NO session but a live group → the same timer tears the
// group down; no session ended, so no summary is written.
TEST(SessionManagerTest, IdleGroupTornDownAfterTimeoutWithoutSummary) {
    FakeCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());

    ASSERT_TRUE(manager.OnConnect());
    ASSERT_TRUE(manager.OnWifiReady());
    manager.OnDisconnect();

    ASSERT_TRUE(WaitFor([&] { return cleanup.teardown_wifi.load() == 1; }));
    const auto after = manager.Snapshot();
    EXPECT_FALSE(after.wifi_group_up);
    EXPECT_EQ(after.phase, SessionPhase::kIdle);
    EXPECT_EQ(cleanup.finalize_recording.load(), 0);
    EXPECT_EQ(cleanup.stop_streaming.load(), 0);
    EXPECT_FALSE(after.last_summary.has_value());
    EXPECT_TRUE(store.Persisted().empty());
}

// Edge: disconnect from Idle with nothing up is a no-op — no timer, no fan-out.
TEST(SessionManagerTest, DisconnectFromIdleIsNoop) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    manager.OnDisconnect();
    std::this_thread::sleep_for(kSettleWait);
    EXPECT_EQ(cleanup.finalize_recording.load(), 0);
    EXPECT_EQ(cleanup.stop_streaming.load(), 0);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 0);
    EXPECT_EQ(cleanup.reset_selections.load(), 0);
}

// ── Finalize gate (CAS) ─────────────────────────────────────────────────────

// The group going down mid-recording finalizes the session FIRST — the old
// data-loss path (drop state, recording never EOSed) is gone.
TEST(SessionManagerTest, WifiStoppedWhileRecordingFinalizes) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    EXPECT_TRUE(manager.OnWifiStopped());

    const auto after = manager.Snapshot();
    EXPECT_EQ(cleanup.finalize_recording.load(), 1);
    EXPECT_EQ(after.phase, SessionPhase::kIdle);
    ASSERT_TRUE(after.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(after.last_summary->end_reason, SessionEndReason::kAppStop);

    fs::remove_all(root);
}

// Error path: a second finalize (command race, auto-stop vs manual stop) loses
// the CAS and is a no-op — single fan-out, single summary.
TEST(SessionManagerTest, DoubleFinalizeIsIdempotent) {
    FakeCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    EXPECT_TRUE(manager.FinalizeSession(SessionEndReason::kAppStop));
    EXPECT_FALSE(manager.FinalizeSession(SessionEndReason::kAutoStop));

    EXPECT_EQ(cleanup.finalize_recording.load(), 1);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);
    // Exactly one REAL summary (the write-ahead from recording start precedes it).
    const auto persisted = store.Persisted();
    ASSERT_FALSE(persisted.empty());
    EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kAppStop);
    int real_summaries = 0;
    for (const auto& entry : persisted) {
        if (entry.end_reason != SessionEndReason::kReboot) {
            ++real_summaries;
        }
    }
    EXPECT_EQ(real_summaries, 1);

    fs::remove_all(root);
}

// A finalize claimed just before a timeout-armed stop could fire: the timer's
// claim loses the CAS — no second fan-out ever runs.
TEST(SessionManagerTest, AutoStopAfterManualFinalizeIsNoop) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    manager.OnDisconnect();                                            // arms the timer
    EXPECT_TRUE(manager.FinalizeSession(SessionEndReason::kAppStop));  // beats it

    std::this_thread::sleep_for(kSettleWait);  // let any stray fire happen
    EXPECT_EQ(cleanup.stop_streaming.load(), 1);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);

    fs::remove_all(root);
}

// A cleanup step that throws must not abort the finalize: the remaining steps
// still run and the session still resets to Idle (exception-safe fan-out); the
// summary reports the file invalid (the finalize never reported success).
class ThrowingFinalizeCleanup final : public FakeCleanup {
   public:
    auto FinalizeRecording() -> bool override {
        ++finalize_recording;
        throw std::runtime_error("recorder blew up on finalize");
    }
};

TEST(SessionManagerTest, FinalizeFanOutIsExceptionSafe) {
    ThrowingFinalizeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    EXPECT_NO_THROW(manager.FinalizeSession(SessionEndReason::kAppStop));

    const auto after = manager.Snapshot();
    EXPECT_EQ(cleanup.finalize_recording.load(), 1);  // attempted
    EXPECT_EQ(cleanup.stop_streaming.load(), 1);      // ran despite the throw
    EXPECT_EQ(cleanup.teardown_wifi.load(), 1);       // ran despite the throw
    EXPECT_EQ(cleanup.reset_selections.load(), 1);    // ran despite the throw
    EXPECT_EQ(after.phase, SessionPhase::kIdle);
    ASSERT_TRUE(after.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_FALSE(after.last_summary->file_valid);

    fs::remove_all(root);
}

// Session memory (config, scores, clock) is cleared at session end — but the
// summary is retained across Idle.
TEST(SessionManagerTest, SessionMemoryClearedOnEndSummaryRetained) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    constexpr std::uint32_t kClockAtEnd = 65;
    ASSERT_TRUE(manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) {
        match.score_a = 2;
        match.score_b = 1;
        match.clock_seconds = kClockAtEnd;
    }));

    ASSERT_TRUE(manager.FinalizeSession(SessionEndReason::kAppStop));

    const auto after = manager.Snapshot();
    EXPECT_EQ(after.phase, SessionPhase::kIdle);
    EXPECT_FALSE(after.config.has_value());
    EXPECT_EQ(after.match.score_a, 0U);
    EXPECT_EQ(after.match.clock_seconds, 0U);
    ASSERT_TRUE(after.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(after.last_summary->end_clock_seconds, kClockAtEnd);

    // A live-match update after the session ended reflects no session.
    EXPECT_FALSE(
        manager.ApplyMatchUpdate([](sst::session::LiveMatch& match) { match.score_a = 9; }));

    fs::remove_all(root);
}

// Edge: a connect landing while a finalize is in flight (timeout+ε) reads
// either the pre-finalize truth (session=Finalizing, config intact) or the
// post-finalize idle-with-summary — never a torn intermediate.
TEST(SessionManagerTest, SnapshotDuringFinalizeIsNeverTorn) {
    // Cleanup that BLOCKS in FinalizeRecording until released, holding the
    // manager mid-fan-out.
    class BlockingCleanup final : public FakeCleanup {
       public:
        auto FinalizeRecording() -> bool override {
            ++finalize_recording;
            std::unique_lock lock(mtx);
            cv.wait(lock, [this] { return released; });
            return true;
        }
        auto Release() -> void {
            {
                const std::lock_guard lock(mtx);
                released = true;
            }
            cv.notify_all();
        }
        std::mutex mtx;
        std::condition_variable cv;
        bool released{false};
    };

    BlockingCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    std::thread finalizer([&manager] { manager.FinalizeSession(SessionEndReason::kAutoStop); });
    ASSERT_TRUE(WaitFor([&] { return cleanup.finalize_recording.load() == 1; }));

    // Mid-finalize: connect accepted; snapshot is the pre-finalize truth.
    EXPECT_TRUE(manager.OnConnect());
    const auto during = manager.Snapshot();
    EXPECT_EQ(during.phase, SessionPhase::kFinalizing);
    ASSERT_TRUE(during.config.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(during.config->match_uuid, "match-uuid");

    cleanup.Release();
    finalizer.join();

    // Post-finalize: idle with the summary readable (AE2 shape).
    const auto after = manager.Snapshot();
    EXPECT_EQ(after.phase, SessionPhase::kIdle);
    EXPECT_TRUE(after.app_connected);
    EXPECT_FALSE(after.config.has_value());
    ASSERT_TRUE(after.last_summary.has_value());

    fs::remove_all(root);
}

// Cleanup that BLOCKS in FinalizeRecording until released — holds the manager
// mid-fan-out so tests can race other calls against an in-flight finalize.
class GateCleanup final : public FakeCleanup {
   public:
    auto FinalizeRecording() -> bool override {
        ++finalize_recording;
        std::unique_lock lock(mtx);
        cv.wait(lock, [this] { return released; });
        return true;
    }
    auto Release() -> void {
        {
            const std::lock_guard lock(mtx);
            released = true;
        }
        cv.notify_all();
    }
    std::mutex mtx;
    std::condition_variable cv;
    bool released{false};
};

// OnWifiReady landing while a finalize is dismantling the session is a no-op:
// accepting it would set wifi_group_up=true only for the post-fan-out reset to
// clobber it back to false while the caller believes the group is live.
TEST(SessionManagerTest, WifiReadyDuringFinalizeIsRejected) {
    GateCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());

    std::thread finalizer([&manager] { manager.FinalizeSession(SessionEndReason::kAppStop); });
    ASSERT_TRUE(WaitFor([&] { return cleanup.finalize_recording.load() == 1; }));

    // Mid-finalize (app still connected): the group-up event is rejected. The
    // snapshot still shows the PRE-finalize truth (group up) — the point is the
    // rejected event doesn't re-assert it past the post-fan-out reset.
    EXPECT_EQ(manager.Phase(), SessionPhase::kFinalizing);
    EXPECT_FALSE(manager.OnWifiReady());

    cleanup.Release();
    finalizer.join();
    EXPECT_FALSE(manager.Snapshot().wifi_group_up);
    EXPECT_EQ(manager.Phase(), SessionPhase::kIdle);

    fs::remove_all(root);
}

// Summary-write ordering: every persist happens under mtx_, so a recording
// stop racing an in-flight finalize can never leave the reboot write-ahead as
// the store's FINAL record — the finalize summary is always last, and the
// racing stop is rejected outright once the phase is Finalizing.
TEST(SessionManagerTest, FinalizeRacingRecordingStopKeepsRealSummaryLast) {
    GateCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());  // write-ahead #1 (reboot/invalid)

    std::thread finalizer([&manager] { manager.FinalizeSession(SessionEndReason::kAutoStop); });
    ASSERT_TRUE(WaitFor([&] { return cleanup.finalize_recording.load() == 1; }));

    // Mid-fan-out, a stop command arrives: the phase is already Finalizing, so
    // it is rejected and persists NO write-ahead behind the finalize's back.
    EXPECT_FALSE(manager.OnRecordingStop());

    cleanup.Release();
    finalizer.join();

    const auto persisted = store.Persisted();
    ASSERT_FALSE(persisted.empty());
    // The final record is the real session end — never the write-ahead.
    EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kAutoStop);
    int write_aheads = 0;
    for (const auto& entry : persisted) {
        if (entry.end_reason == SessionEndReason::kReboot) {
            ++write_aheads;
        }
    }
    EXPECT_EQ(write_aheads, 1);  // only the one from OnRecordingStart

    fs::remove_all(root);
}

// ── Elapsed time + summary persistence ──────────────────────────────────────

// Recording elapsed seconds are tracked monotonically and survive a disconnect
// (the U2 snapshot shows the app how long it has been recording after rejoin).
TEST(SessionManagerTest, RecordingElapsedSecondsTracked) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    EXPECT_EQ(manager.Snapshot().recording_elapsed_seconds, 0U);
    ASSERT_TRUE(manager.OnRecordingStart());
    manager.OnDisconnect();  // elapsed keeps counting while the app is away
    EXPECT_TRUE(manager.OnConnect());

    constexpr std::chrono::milliseconds kRecordSpan{1100};
    std::this_thread::sleep_for(kRecordSpan);
    EXPECT_GE(manager.Snapshot().recording_elapsed_seconds, 1U);
    ASSERT_TRUE(manager.OnRecordingStop());
    const auto stopped_elapsed = manager.Snapshot().recording_elapsed_seconds;
    EXPECT_GE(stopped_elapsed, 1U);

    fs::remove_all(root);
}

// Boot reconciliation: the summary the store loads (e.g. the write-ahead reboot
// record of a crashed session) is visible in the snapshot from construction.
TEST(SessionManagerTest, BootLoadsPersistedSummary) {
    FakeCleanup cleanup;
    FakeStore store;
    LastSessionSummary orphan;
    orphan.match_uuid = "crashed-match";
    orphan.end_reason = SessionEndReason::kReboot;
    orphan.file_valid = false;
    store.preloaded = orphan;

    SessionManager manager(cleanup, &store, TestTiming());
    const auto snap = manager.Snapshot();
    EXPECT_EQ(snap.phase, SessionPhase::kIdle);  // reboot looks like a first connect
    EXPECT_FALSE(snap.config.has_value());
    ASSERT_TRUE(snap.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    const auto& summary = *snap.last_summary;
    EXPECT_EQ(summary.match_uuid, "crashed-match");
    EXPECT_EQ(summary.end_reason, SessionEndReason::kReboot);
    EXPECT_FALSE(summary.file_valid);
}

// Write-ahead sequence: recording start persists a reboot/file-invalid record
// (the crash orphan truth), a clean commanded stop upgrades it to file-valid,
// and the session end overwrites it with the real reason.
TEST(SessionManagerTest, WriteAheadSummaryTracksRecordingLifecycle) {
    FakeCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());
    {
        const auto persisted = store.Persisted();
        ASSERT_EQ(persisted.size(), 1U);
        EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kReboot);
        EXPECT_FALSE(persisted.back().file_valid);
        EXPECT_EQ(persisted.back().match_uuid, "match-uuid");
    }
    ASSERT_TRUE(manager.OnRecordingStop());
    {
        const auto persisted = store.Persisted();
        ASSERT_EQ(persisted.size(), 2U);
        EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kReboot);
        EXPECT_TRUE(persisted.back().file_valid);
    }
    ASSERT_TRUE(manager.FinalizeSession(SessionEndReason::kAppStop));
    {
        const auto persisted = store.Persisted();
        ASSERT_EQ(persisted.size(), 3U);
        EXPECT_EQ(persisted.back().end_reason, SessionEndReason::kAppStop);
        EXPECT_TRUE(persisted.back().file_valid);  // clean stop happened earlier
    }

    fs::remove_all(root);
}

// Integration: full connect → configure → record → disconnect → auto-stop →
// reconnect. Ends with an idle snapshot + readable summary (F1/F2 end-to-end).
TEST(SessionManagerTest, FullCycleEndsIdleWithSummary) {
    FakeCleanup cleanup;
    FakeStore store;
    SessionManager manager(cleanup, &store, TestTiming());
    const fs::path root = MakeTempRoot();
    const auto cfg = MakeConfig(root);

    AdvanceToReady(manager, cfg);
    ASSERT_TRUE(manager.OnRecordingStart());
    manager.OnDisconnect();
    ASSERT_TRUE(WaitFor([&] { return manager.Phase() == SessionPhase::kIdle; }));

    EXPECT_TRUE(manager.OnConnect());  // rejoin after the auto-stop
    const auto snap = manager.Snapshot();
    EXPECT_TRUE(snap.app_connected);
    EXPECT_FALSE(snap.wifi_group_up);
    EXPECT_EQ(snap.phase, SessionPhase::kIdle);
    EXPECT_FALSE(snap.config.has_value());
    ASSERT_TRUE(snap.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    const auto& summary = *snap.last_summary;
    EXPECT_EQ(summary.match_uuid, "match-uuid");
    EXPECT_EQ(summary.end_reason, SessionEndReason::kAutoStop);

    fs::remove_all(root);
}

}  // namespace
