// U3 frame-truth per-camera health: OK / RECOVERING / DOWN derived from actual
// frame flow + watchdog restart outcomes, the one-shot DOWN notification, and
// the DOWN-while-recording → camera-failure finalize chain. The orchestrator's
// threading is exercised for real with thread-safe capture doubles; the health
// getters are hammered from the test thread while producers run (the same
// cross-thread read pattern the BLE dispatcher uses in production).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/capture/ports/frame-src.hpp"
#include "app/decision/services/static_decision/static-decision.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/processing/ports/postprocessor.hpp"
#include "app/processing/ports/preprocessor.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/health/models/camera-health.hpp"
#include "domain/health/models/formatter/_fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/frame-bundle.hpp"
#include "domain/session/models/session-config.hpp"
#include "domain/session/models/session-summary.hpp"

namespace {

using sst::capture::Frame;
using sst::capture::ICaptureFrame;
using sst::decision::StaticDecision;
using sst::health::CameraHealth;
using sst::pipeline::CameraChain;
using sst::pipeline::PipelineConfig;
using sst::pipeline::PipelineOrchestrator;
using sst::processing::CropRect;
using sst::processing::FrameBundle;
using sst::processing::IPostprocessor;
using sst::processing::IPreprocessor;

// NOLINTBEGIN(readability-magic-numbers) — these *are* the named definitions
constexpr int kIdlePollMs = 2;
constexpr int kFramePeriodMs = 10;
constexpr auto kTestTimeout = std::chrono::seconds(5);
constexpr auto kSamplePeriod = std::chrono::milliseconds(1);
// NOLINTEND(readability-magic-numbers)

// Capture double mirroring the real GStreamerAdapter's U3 semantics:
//  - Start() reports PRIMED truth (a scripted prime failure leaves it down);
//  - a successful Start() records a prime sample (frame truth);
//  - Stop() resets the frame truth (a restarting camera reads stalled);
//  - Capture() records frame truth on every delivered frame;
//  - Kill(k) simulates the on-device death (HandleBusMessages()->Stop()):
//    IsRunning flips false and the next k Start() attempts fail (a settling
//    radio / cold nvargus-daemon), after which Start() succeeds again;
//  - SetStalled(true) simulates a silent stall: still "running", no frames —
//    the watchdog never fires, only frame-age health sees it.
class HealthFakeCapture final : public ICaptureFrame {
   public:
    explicit HealthFakeCapture(int initial_start_failures = 0)
        : start_failures_remaining_(initial_start_failures) {}

    auto Start() -> void override {
        start_calls_.fetch_add(1);
        if (start_failures_remaining_.load() > 0) {
            start_failures_remaining_.fetch_sub(1);
            return;  // failed to prime — reported truth stays "not running"
        }
        RecordSample();  // the prime frame
        running_ = true;
    }

    auto Stop() -> void override {
        running_ = false;
        last_sample_ns_ = kNoSample;  // frame truth resets with the pipeline
    }

    [[nodiscard]] auto IsRunning() const -> bool override { return running_; }

    auto Capture() -> std::optional<Frame> override {
        if (!running_ || stalled_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
            return std::nullopt;
        }
        RecordSample();
        Frame frame;
        frame.frame_id = next_id_.fetch_add(1);
        frame.format = sst::common::PixelFormat::NV12;
        frame.geometry = {.width = 640, .height = 360};  // NOLINT(readability-magic-numbers)
        std::this_thread::sleep_for(std::chrono::milliseconds(kFramePeriodMs));
        return frame;
    }

    [[nodiscard]] auto LastSampleAt() const
        -> std::optional<std::chrono::steady_clock::time_point> override {
        const std::int64_t nanoseconds = last_sample_ns_;
        if (nanoseconds == kNoSample) {
            return std::nullopt;
        }
        return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanoseconds}};
    }

    // Simulate mid-run pipeline death; the next `failed_restart_attempts`
    // watchdog Restart()s complete and fail before one succeeds.
    auto Kill(int failed_restart_attempts) -> void {
        start_failures_remaining_ = failed_restart_attempts;
        running_ = false;
    }
    auto SetStalled(bool stalled) -> void { stalled_ = stalled; }
    [[nodiscard]] auto StartCalls() const -> int { return start_calls_.load(); }

   private:
    static constexpr std::int64_t kNoSample = -1;

    auto RecordSample() -> void {
        last_sample_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> stalled_{false};
    std::atomic<int> start_failures_remaining_{0};
    std::atomic<int> start_calls_{0};
    std::atomic<std::uint64_t> next_id_{0};
    std::atomic<std::int64_t> last_sample_ns_{kNoSample};
};

class PassPreprocessor final : public IPreprocessor {
   public:
    auto Process(const Frame& raw) -> std::optional<FrameBundle> override {
        FrameBundle bundle;
        bundle.source_frame = raw;
        bundle.ai_frame = raw;
        return bundle;
    }
};

class PassPostprocessor final : public IPostprocessor {
   public:
    auto Process(const Frame& source, const CropRect& /*crop*/) -> std::optional<Frame> override {
        return source;
    }
};

class NullSink final : public sst::buffer::IFrameSink {
   public:
    auto Push(const Frame& /*frame*/) -> void override {}
};

// Fast health thresholds: DOWN after 3 completed-and-failed restarts; frames
// older than 150ms read stalled. Restart backoff 50ms keeps a scripted failure
// window (attempts x backoff) long enough to sample RECOVERING reliably even
// under the slower qemu CI emulation.
constexpr int kDownAfterFailedRestarts = 3;
auto HealthConfig() -> PipelineConfig {
    // NOLINTBEGIN(readability-magic-numbers) — self-evident test timings
    return PipelineConfig{.capture_idle_sleep = std::chrono::milliseconds(1),
                          .consumer_pop_timeout = std::chrono::milliseconds(20),
                          .capture_restart_backoff = std::chrono::milliseconds(50),
                          .health_stall_threshold = std::chrono::milliseconds(150),
                          .health_down_after_failed_restarts = kDownAfterFailedRestarts};
    // NOLINTEND(readability-magic-numbers)
}

auto TwoCameras(std::unique_ptr<ICaptureFrame> cam0,
                std::unique_ptr<ICaptureFrame> cam1) -> std::vector<CameraChain> {
    std::vector<CameraChain> chains;
    chains.push_back(CameraChain{.capture = std::move(cam0),
                                 .preprocessor = std::make_unique<PassPreprocessor>()});
    chains.push_back(CameraChain{.capture = std::move(cam1),
                                 .preprocessor = std::make_unique<PassPreprocessor>()});
    return chains;
}

// Poll health until `want` is observed (or timeout); returns true on success.
auto WaitForHealth(const PipelineOrchestrator& orchestrator, std::size_t camera_index,
                   CameraHealth want) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (orchestrator.CameraHealthStatus(camera_index) == want) {
            return true;
        }
        std::this_thread::sleep_for(kSamplePeriod);
    }
    return false;
}

// Health values observed while waiting for `until` (which is also recorded).
auto ObserveUntil(const PipelineOrchestrator& orchestrator, std::size_t camera_index,
                  CameraHealth until) -> std::set<CameraHealth> {
    std::set<CameraHealth> seen;
    const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto health = orchestrator.CameraHealthStatus(camera_index);
        if (health) {
            seen.insert(*health);
            if (*health == until) {
                break;
            }
        }
        std::this_thread::sleep_for(kSamplePeriod);
    }
    return seen;
}

// ── Health derivation ────────────────────────────────────────────────

// Repo convention: every domain model ships a fmt::formatter. Pin the names so
// log lines stay greppable.
TEST(PipelineHealthTest, CameraHealthFormatterNamesStates) {
    EXPECT_EQ(fmt::format("{}", CameraHealth::kOk), "Ok");
    EXPECT_EQ(fmt::format("{}", CameraHealth::kRecovering), "Recovering");
    EXPECT_EQ(fmt::format("{}", CameraHealth::kDown), "Down");
}

// Happy path: frames flowing on both cameras → both OK.
TEST(PipelineHealthTest, BothCamerasFlowingReadOk) {
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<HealthFakeCapture>(), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());

    ASSERT_TRUE(orchestrator.Start());
    EXPECT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));
    EXPECT_TRUE(WaitForHealth(orchestrator, 1, CameraHealth::kOk));
    orchestrator.Stop();
}

// An out-of-range camera index reads "unreported", never a fabricated value.
TEST(PipelineHealthTest, OutOfRangeCameraIndexReadsNullopt) {
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<HealthFakeCapture>(), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    EXPECT_FALSE(orchestrator.CameraHealthStatus(2).has_value());
}

// Before Start() no frames ever flowed → RECOVERING (stalled, no failed
// restarts), not a fabricated OK — gating must refuse starts against a
// not-yet-started pipeline.
TEST(PipelineHealthTest, BeforeStartReadsRecovering) {
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<HealthFakeCapture>(), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    EXPECT_EQ(orchestrator.CameraHealthStatus(0), CameraHealth::kRecovering);
    EXPECT_EQ(orchestrator.CameraHealthStatus(1), CameraHealth::kRecovering);
}

// Anti-flap: a silent stall (still "running", no frames — the watchdog never
// fires) reads RECOVERING once the stall threshold passes, then returns to OK
// when frames resume. DOWN is never reported and no DOWN notification fires.
TEST(PipelineHealthTest, StallThenRecoveryIsRecoveringThenOkNeverDown) {
    auto cam1_owner = std::make_unique<HealthFakeCapture>();
    auto* cam1 = cam1_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<HealthFakeCapture>(), std::move(cam1_owner)),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    std::atomic<int> down_calls{0};
    orchestrator.SetOnCameraDown(
        [&down_calls](std::size_t /*camera*/) { down_calls.fetch_add(1); });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 1, CameraHealth::kOk));

    cam1->SetStalled(true);
    const auto seen_stalling = ObserveUntil(orchestrator, 1, CameraHealth::kRecovering);
    EXPECT_TRUE(seen_stalling.contains(CameraHealth::kRecovering));
    EXPECT_FALSE(seen_stalling.contains(CameraHealth::kDown));

    cam1->SetStalled(false);
    const auto seen_recovering = ObserveUntil(orchestrator, 1, CameraHealth::kOk);
    EXPECT_TRUE(seen_recovering.contains(CameraHealth::kOk));
    EXPECT_FALSE(seen_recovering.contains(CameraHealth::kDown));
    EXPECT_EQ(down_calls.load(), 0);
    orchestrator.Stop();
}

// U3 Start() tolerance, dual-camera shape: camera 1 fails its boot prime (cold
// nvargus-daemon) and its first watchdog restart, then heals. The orchestrator
// starts anyway, camera 0 is never rolled back, camera 1 begins RECOVERING and
// the watchdog recovers it to OK.
TEST(PipelineHealthTest, PrimeTimeoutAtBootStartsRecoveringAndWatchdogHeals) {
    // 2 scripted failures: the boot Start() consumes one, one watchdog restart
    // fails, the next succeeds — always below the DOWN threshold of 3.
    auto cam1_owner = std::make_unique<HealthFakeCapture>(/*initial_start_failures=*/2);
    auto* cam1 = cam1_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<HealthFakeCapture>(), std::move(cam1_owner)),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    std::atomic<int> down_calls{0};
    orchestrator.SetOnCameraDown(
        [&down_calls](std::size_t /*camera*/) { down_calls.fetch_add(1); });

    ASSERT_TRUE(orchestrator.Start()) << "an unprimed camera must not abort Start()";
    EXPECT_TRUE(orchestrator.IsRunning());
    // The unprimed camera reads RECOVERING (no frames yet), never OK-by-fiat.
    EXPECT_EQ(orchestrator.CameraHealthStatus(1), CameraHealth::kRecovering);
    // Camera 0 was not rolled back and flows normally.
    EXPECT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));
    // The watchdog heals camera 1 without it ever reaching DOWN.
    const auto seen = ObserveUntil(orchestrator, 1, CameraHealth::kOk);
    EXPECT_TRUE(seen.contains(CameraHealth::kOk));
    EXPECT_FALSE(seen.contains(CameraHealth::kDown));
    EXPECT_EQ(down_calls.load(), 0);
    EXPECT_GE(cam1->StartCalls(), 3);  // boot prime + failed restart + success
    orchestrator.Stop();
}

// Integration shape of the watchdog window: a mid-run death whose restarts fail
// a couple of times (below N) produces RECOVERING — never a DOWN→UP flap —
// and returns to OK when a restart finally primes.
TEST(PipelineHealthTest, WatchdogRestartSequenceIsRecoveringNotDownUpFlap) {
    auto cam0_owner = std::make_unique<HealthFakeCapture>();
    auto* cam0 = cam0_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::move(cam0_owner), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    std::atomic<int> down_calls{0};
    orchestrator.SetOnCameraDown(
        [&down_calls](std::size_t /*camera*/) { down_calls.fetch_add(1); });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));

    cam0->Kill(/*failed_restart_attempts=*/kDownAfterFailedRestarts - 1);
    // Phase 1: the episode is observed as RECOVERING (the watchdog Restart()'s
    // Stop() resets frame truth immediately) with no DOWN reading on the way.
    const auto seen_dying = ObserveUntil(orchestrator, 0, CameraHealth::kRecovering);
    EXPECT_TRUE(seen_dying.contains(CameraHealth::kRecovering));
    EXPECT_FALSE(seen_dying.contains(CameraHealth::kDown));
    // Phase 2: the camera returns to OK — still without ever reading DOWN.
    const auto seen_recovering = ObserveUntil(orchestrator, 0, CameraHealth::kOk);
    EXPECT_TRUE(seen_recovering.contains(CameraHealth::kOk));
    EXPECT_FALSE(seen_recovering.contains(CameraHealth::kDown));
    EXPECT_EQ(down_calls.load(), 0);
    orchestrator.Stop();
}

// DOWN is a count of completed-and-failed restarts, and the notification is
// one-shot per episode: N failed restarts → DOWN + exactly one callback; frames
// resuming return the camera to OK.
TEST(PipelineHealthTest, DownAfterNFailedRestartsFiresOnceAndFramesResumeToOk) {
    auto cam0_owner = std::make_unique<HealthFakeCapture>();
    auto* cam0 = cam0_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::move(cam0_owner), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    constexpr std::size_t kNoCameraReported = 99;
    std::atomic<int> down_calls{0};
    std::atomic<std::size_t> down_camera{kNoCameraReported};
    orchestrator.SetOnCameraDown([&down_calls, &down_camera](std::size_t camera) {
        down_calls.fetch_add(1);
        down_camera = camera;
    });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));

    cam0->Kill(/*failed_restart_attempts=*/kDownAfterFailedRestarts + 2);
    EXPECT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kDown));
    // Recovery: the scripted failures run out, a restart primes, frames flow.
    EXPECT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));
    EXPECT_EQ(down_calls.load(), 1) << "DOWN notification must be one-shot per episode";
    EXPECT_EQ(down_camera.load(), 0U);
    // The healthy camera was never disturbed.
    EXPECT_EQ(orchestrator.CameraHealthStatus(1), CameraHealth::kOk);
    orchestrator.Stop();
}

// Serialized restarts must not false-DOWN the queued camera: both cameras die
// at once (the radio-reform shape), each needs one failed restart before
// healing, and camera B's recovery completes well after camera A's (restarts
// serialize on restart_mtx_). Neither reads DOWN, nothing fires.
TEST(PipelineHealthTest, SimultaneousStallsSerializedRestartsNeverReadDown) {
    auto cam0_owner = std::make_unique<HealthFakeCapture>();
    auto cam1_owner = std::make_unique<HealthFakeCapture>();
    auto* cam0 = cam0_owner.get();
    auto* cam1 = cam1_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(TwoCameras(std::move(cam0_owner), std::move(cam1_owner)),
                                      std::make_unique<PassPostprocessor>(),
                                      std::make_unique<StaticDecision>(), record_sink, stream_sink,
                                      HealthConfig());
    std::atomic<int> down_calls{0};
    orchestrator.SetOnCameraDown(
        [&down_calls](std::size_t /*camera*/) { down_calls.fetch_add(1); });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));
    ASSERT_TRUE(WaitForHealth(orchestrator, 1, CameraHealth::kOk));

    cam0->Kill(/*failed_restart_attempts=*/1);
    cam1->Kill(/*failed_restart_attempts=*/1);
    // Camera 0's episode is observed strictly: RECOVERING first, then OK,
    // never DOWN. Camera 1 recovers later (its restart queues behind camera
    // 0's on restart_mtx_ — ~2x a single window) and must also end OK with no
    // DOWN ever reported (a DOWN reading would have fired the callback below).
    const auto seen_dying = ObserveUntil(orchestrator, 0, CameraHealth::kRecovering);
    EXPECT_FALSE(seen_dying.contains(CameraHealth::kDown));
    const auto seen0 = ObserveUntil(orchestrator, 0, CameraHealth::kOk);
    const auto seen1 = ObserveUntil(orchestrator, 1, CameraHealth::kOk);
    EXPECT_TRUE(seen0.contains(CameraHealth::kOk));
    EXPECT_TRUE(seen1.contains(CameraHealth::kOk));
    EXPECT_FALSE(seen0.contains(CameraHealth::kDown));
    EXPECT_FALSE(seen1.contains(CameraHealth::kDown));
    EXPECT_EQ(down_calls.load(), 0);
    orchestrator.Stop();
}

// ── DOWN-while-recording → camera-failure finalize (session hook) ────

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override {
        ++finalize_calls;
        return true;
    }
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}

    int finalize_calls{0};
};

auto RecordingSessionConfig() -> sst::session::SessionConfig {
    sst::session::SessionConfig config;
    config.match_uuid = "match-health";
    config.user_uuid = "user";
    return config;
}

auto AdvanceToRecording(sst::session::SessionManager& session) -> void {
    ASSERT_TRUE(session.OnConnect());
    ASSERT_TRUE(session.OnWifiReady());
    ASSERT_TRUE(session.ApplySessionConfig(RecordingSessionConfig()));
    ASSERT_TRUE(session.OnOverlayConfigured());
    ASSERT_TRUE(session.OnRecordingStart());
}

// Camera DOWN mid-recording finalizes the session cleanly, exactly once, with
// end-reason = camera failure (wired the same way main.cpp wires it).
TEST(PipelineHealthTest, CameraDownMidRecordingFinalizesWithCameraFailure) {
    FakeCleanup cleanup;
    sst::session::SessionManager session(cleanup);
    AdvanceToRecording(session);

    auto cam0_owner = std::make_unique<HealthFakeCapture>();
    auto* cam0 = cam0_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::move(cam0_owner), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    orchestrator.SetOnCameraDown([&session](std::size_t /*camera*/) {
        if (session.Phase() == sst::session::SessionPhase::kRecording) {
            session.FinalizeSession(sst::session::SessionEndReason::kCameraFailure);
        }
    });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));

    // Kill camera 0 hard enough to cross the DOWN threshold.
    cam0->Kill(/*failed_restart_attempts=*/kDownAfterFailedRestarts + 2);

    const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           session.Phase() != sst::session::SessionPhase::kIdle) {
        std::this_thread::sleep_for(kSamplePeriod);
    }
    orchestrator.Stop();

    ASSERT_EQ(session.Phase(), sst::session::SessionPhase::kIdle);
    EXPECT_EQ(cleanup.finalize_calls, 1) << "finalize is CAS-claimed — exactly once";
    const auto snapshot = session.Snapshot();
    ASSERT_TRUE(snapshot.last_summary.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    const auto& summary = *snapshot.last_summary;
    EXPECT_EQ(summary.end_reason, sst::session::SessionEndReason::kCameraFailure);
    EXPECT_TRUE(summary.file_valid) << "recording was finalized cleanly";
}

// Anti-flap on the session: a stall that recovers within the watchdog window
// (never DOWN) leaves a running recording completely untouched.
TEST(PipelineHealthTest, StallRecoveryWithinWindowLeavesRecordingUntouched) {
    FakeCleanup cleanup;
    sst::session::SessionManager session(cleanup);
    AdvanceToRecording(session);

    auto cam0_owner = std::make_unique<HealthFakeCapture>();
    auto* cam0 = cam0_owner.get();
    NullSink record_sink;
    NullSink stream_sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::move(cam0_owner), std::make_unique<HealthFakeCapture>()),
        std::make_unique<PassPostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        stream_sink, HealthConfig());
    orchestrator.SetOnCameraDown([&session](std::size_t /*camera*/) {
        if (session.Phase() == sst::session::SessionPhase::kRecording) {
            session.FinalizeSession(sst::session::SessionEndReason::kCameraFailure);
        }
    });

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));

    // One failed restart — recovers within the window, below the threshold.
    cam0->Kill(/*failed_restart_attempts=*/1);
    // The watchdog's Restart() resets frame truth, so the episode is observable
    // as RECOVERING before frames resume to OK.
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kRecovering));
    ASSERT_TRUE(WaitForHealth(orchestrator, 0, CameraHealth::kOk));
    orchestrator.Stop();

    EXPECT_EQ(session.Phase(), sst::session::SessionPhase::kRecording)
        << "transient stall must not end the session";
    EXPECT_EQ(cleanup.finalize_calls, 0);
    // Leave the session finalized so the SessionManager destructor's safety
    // net doesn't have to (keeps the assertion surface explicit).
    session.FinalizeSession(sst::session::SessionEndReason::kAppStop);
}

}  // namespace
