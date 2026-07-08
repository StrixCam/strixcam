#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/capture/ports/frame-src.hpp"
#include "app/decision/services/manual_decision/manual-decision.hpp"
#include "app/decision/services/static_decision/static-decision.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/processing/ports/frame-compositor.hpp"
#include "app/processing/ports/postprocessor.hpp"
#include "app/processing/ports/preprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/frame-bundle.hpp"
#include "domain/streaming/models/preview-layout.hpp"

namespace {

using sst::buffer::IFrameSink;
using sst::capture::Frame;
using sst::capture::ICaptureFrame;
using sst::decision::ManualCameraState;
using sst::decision::ManualDecision;
using sst::decision::StaticDecision;
using sst::pipeline::CameraChain;
using sst::pipeline::PipelineConfig;
using sst::pipeline::PipelineOrchestrator;
using sst::processing::CropRect;
using sst::processing::FrameBundle;
using sst::processing::IPostprocessor;
using sst::processing::IPreprocessor;

// Frame dimensions bundled so width/height can't be transposed at a call site
// (and so FakeCapture takes one geometry argument, not two swappable ones).
struct Dims {
    std::uint32_t width;
    std::uint32_t height;
};

// Named geometries used across the tests. kCam0/kCam1 are deliberately distinct
// so a test can tell which camera's bundle reached postprocess. kOutput is the
// fixed size FakePostprocessor stamps on every final frame.
// NOLINTBEGIN(readability-magic-numbers) — these *are* the named definitions
constexpr Dims kCam0Dims{640, 360};
constexpr Dims kCam1Dims{800, 600};
constexpr Dims kOutputDims{1280, 720};

// FakeCapture timings. kIdlePollMs avoids a hot spin while stalled/stopped;
// kFramePeriodMs paces ~30fps so the producer doesn't starve the consumer.
constexpr int kIdlePollMs = 5;
constexpr int kFramePeriodMs = 33;
// NOLINTEND(readability-magic-numbers)

// ── Test doubles ─────────────────────────────────────────────────────
//
// The real GStreamerAdapter / OpenCv* adapters need IMX477 / OpenCV runtime;
// here we substitute thread-safe doubles so the orchestrator's threading +
// Start/Stop semantics are tested end-to-end without hardware dependencies.

class FakeCapture final : public ICaptureFrame {
   public:
    FakeCapture() = default;
    // Per-camera geometry lets dual-camera tests tell which camera's frame
    // reached postprocess. `stall` makes Capture() always return nullopt (the
    // camera produces nothing) without stopping the producer thread.
    // id_base offsets this camera's frame_id sequence so a test can tell which
    // camera a composited pane came from (frame_ids otherwise overlap at 0).
    explicit FakeCapture(Dims dims, bool stall = false, std::uint64_t id_base = 0)
        : width_(dims.width), height_(dims.height), stall_(stall), next_id_(id_base) {}

    auto Start() -> void override {
        running_ = true;
        start_calls_.fetch_add(1);
    }
    auto Stop() -> void override { running_ = false; }
    [[nodiscard]] auto IsRunning() const -> bool override { return running_; }

    // Simulate the on-device failure the watchdog exists for: after `die_after`
    // frames the pipeline dies once (mirrors HandleBusMessages()->Stop() on an
    // Argus ERROR — IsRunning() flips false, Capture() returns nothing) and stays
    // dead until something calls Start()/Restart() again. One-shot so a successful
    // restart runs clean forever after.
    auto SetDieAfter(std::uint64_t die_after) -> void { die_after_ = die_after; }
    [[nodiscard]] auto StartCalls() const -> int { return start_calls_.load(); }

    // Runtime stall toggle (U4): flip the camera between producing and fully
    // stalled mid-test — mirrors a sensor stall the producer watchdog is still
    // working (RECOVERING) without killing the producer thread.
    auto SetStall(bool stall) -> void { stall_ = stall; }
    // Frame pacing override (U4): a camera slower than the consumer cadence
    // makes the non-cadence TryPop miss on a steady fraction of ticks —
    // the alternating-miss pattern behind the both-mode flicker.
    auto SetFramePeriod(std::chrono::milliseconds period) -> void { frame_period_ = period; }

    auto Capture() -> std::optional<Frame> override {
        if (running_ && die_after_ > 0 && !has_died_ && next_id_.load() >= die_after_) {
            has_died_ = true;
            running_ = false;  // pipeline death — watchdog must Restart() to recover
        }
        if (!running_ || stall_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
            return std::nullopt;
        }
        Frame frame;
        frame.frame_id = next_id_.fetch_add(1);
        frame.format = sst::common::PixelFormat::NV12;
        frame.geometry = {.width = width_, .height = height_};
        // Match real GStreamerAdapter behavior: pace at ~30fps so the producer
        // doesn't spin on Capture() in the test, and to give the consumer a
        // chance to drain the slot between pushes.
        std::this_thread::sleep_for(frame_period_);
        return frame;
    }

   private:
    std::uint32_t width_{kCam0Dims.width};
    std::uint32_t height_{kCam0Dims.height};
    std::atomic<bool> stall_{false};
    std::chrono::milliseconds frame_period_{kFramePeriodMs};
    std::uint64_t die_after_{0};  // 0 means never die
    std::atomic<bool> has_died_{false};
    std::atomic<int> start_calls_{0};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_id_{0};
};

class FakePreprocessor final : public IPreprocessor {
   public:
    auto Process(const Frame& raw) -> std::optional<FrameBundle> override {
        ++process_calls;
        if (refuse_after_n_ > 0 && process_calls > refuse_after_n_) {
            return std::nullopt;
        }
        FrameBundle bundle;
        bundle.source_frame = raw;  // keeps owner shared_ptr (nullptr in test)
        bundle.ai_frame = raw;
        return bundle;
    }

    std::atomic<int> process_calls{0};
    int refuse_after_n_{0};  // 0 means never refuse
};

class FakePostprocessor final : public IPostprocessor {
   public:
    auto Process(const Frame& source, const CropRect& crop,
                 std::size_t /*camera_index*/) -> std::optional<Frame> override {
        std::lock_guard lock(mtx_);
        ++process_calls;
        last_crop = crop;
        last_source_geometry = source.geometry;

        Frame out = source;
        out.format = sst::common::PixelFormat::BGR8;
        out.geometry = {.width = kOutputDims.width, .height = kOutputDims.height};
        return out;
    }

    int process_calls{0};
    CropRect last_crop{};
    sst::capture::FrameGeometry last_source_geometry{};

   private:
    mutable std::mutex mtx_;
};

class CountingSink final : public IFrameSink {
   public:
    auto Push(const Frame& frame) -> void override {
        std::lock_guard lock(mtx_);
        ++push_calls;
        last_frame_id = frame.frame_id;
        last_format = frame.format;
        last_geometry = frame.geometry;
        widths_.push_back(frame.geometry.width);
    }

    auto Snapshot() const
        -> std::tuple<int, std::uint64_t, sst::common::PixelFormat, sst::capture::FrameGeometry> {
        std::lock_guard lock(mtx_);
        return {push_calls, last_frame_id, last_format, last_geometry};
    }

    // U4 helpers: every pushed frame's width is recorded, so a test can assert
    // over the WHOLE stream ("zero bare single-camera frames ever reached the
    // sink"), not just the last frame observed.
    auto PushCount() const -> int {
        std::lock_guard lock(mtx_);
        return push_calls;
    }
    auto CountPushesWithWidth(std::uint32_t width) const -> int {
        std::lock_guard lock(mtx_);
        return static_cast<int>(std::count(widths_.begin(), widths_.end(), width));
    }

   private:
    mutable std::mutex mtx_;
    int push_calls{0};
    std::uint64_t last_frame_id{0};
    sst::common::PixelFormat last_format{};
    sst::capture::FrameGeometry last_geometry{};
    std::vector<std::uint32_t> widths_;
};

// Records every CompositeSideBySide call and returns a frame stamped with a
// distinctive geometry, so a test can prove the composite (not the single
// postprocessed frame) reached the stream sink (#6 F6d).
constexpr Dims kCompositeDims{2222, 720};
class FakeCompositor final : public sst::processing::IFrameCompositor {
   public:
    // One recorded composite call: both pane ids + the right pane's geometry,
    // so U4 tests can prove which pane advanced and which held across ticks.
    struct PaneRecord {
        std::uint64_t left_id{0};
        std::uint64_t right_id{0};
        sst::capture::FrameGeometry right_geometry{};
    };

    // NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: IFrameCompositor contract
    auto CompositeSideBySide(const Frame& left,
                             const Frame& right) -> std::optional<Frame> override {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        std::lock_guard lock(mtx_);
        ++calls;
        records_.push_back(PaneRecord{.left_id = left.frame_id,
                                      .right_id = right.frame_id,
                                      .right_geometry = right.geometry});
        if (fail_) {
            return std::nullopt;  // U4: simulate CompositeSideBySide nullopt
        }
        Frame out = left;
        out.format = sst::common::PixelFormat::BGR8;
        out.geometry = {.width = kCompositeDims.width, .height = kCompositeDims.height};
        return out;
    }

    // U4: make every subsequent composite fail (nullopt) — the orchestrator
    // must follow the hold path instead of degrading to a single frame.
    auto SetFail(bool fail) -> void {
        std::lock_guard lock(mtx_);
        fail_ = fail;
    }

    auto Calls() const -> int {
        std::lock_guard lock(mtx_);
        return calls;
    }
    auto LastRightGeometry() const -> sst::capture::FrameGeometry {
        std::lock_guard lock(mtx_);
        return records_.empty() ? sst::capture::FrameGeometry{} : records_.back().right_geometry;
    }
    auto LastLeftId() const -> std::uint64_t {
        std::lock_guard lock(mtx_);
        return records_.empty() ? 0 : records_.back().left_id;
    }
    auto Records() const -> std::vector<PaneRecord> {
        std::lock_guard lock(mtx_);
        return records_;
    }

   private:
    mutable std::mutex mtx_;
    int calls{0};
    bool fail_{false};
    std::vector<PaneRecord> records_;
};

// ── Construction helpers ─────────────────────────────────────────────
//
// The orchestrator is non-movable, so we build the camera-chain vector with
// these helpers and pass it straight into the constructor. Single-camera tests
// use OneCamera + a real StaticDecision (deterministic: cam 0 full-frame).

auto OneCamera(std::unique_ptr<ICaptureFrame> capture,
               std::unique_ptr<IPreprocessor> preprocessor) -> std::vector<CameraChain> {
    std::vector<CameraChain> chains;
    chains.push_back(
        CameraChain{.capture = std::move(capture), .preprocessor = std::move(preprocessor)});
    return chains;
}

auto TwoCameras(std::unique_ptr<ICaptureFrame> cam0, std::unique_ptr<IPreprocessor> pre0,
                std::unique_ptr<ICaptureFrame> cam1,
                std::unique_ptr<IPreprocessor> pre1) -> std::vector<CameraChain> {
    std::vector<CameraChain> chains;
    chains.push_back(CameraChain{.capture = std::move(cam0), .preprocessor = std::move(pre0)});
    chains.push_back(CameraChain{.capture = std::move(cam1), .preprocessor = std::move(pre1)});
    return chains;
}

// Spin up the orchestrator with fast polling so tests run in well under a
// second. Real production timings are 5/100ms; tests use 1/20ms.
auto FastConfig() -> PipelineConfig {
    return PipelineConfig{
        .capture_idle_sleep = std::chrono::milliseconds(1),
        .consumer_pop_timeout = std::chrono::milliseconds(
            20),  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    };
}

// ── Tests ────────────────────────────────────────────────────────────

TEST(PipelineOrchestratorTest, StartIsIdempotentAndReturnsRunning) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    EXPECT_FALSE(orchestrator.IsRunning());
    EXPECT_TRUE(orchestrator.Start());
    EXPECT_TRUE(orchestrator.IsRunning());

    EXPECT_TRUE(orchestrator.Start());  // idempotent
    EXPECT_TRUE(orchestrator.IsRunning());

    orchestrator.Stop();
    EXPECT_FALSE(orchestrator.IsRunning());
}

// U3 Start() tolerance: a camera that never primes (cold nvargus-daemon boot)
// must NOT abort the start — the orchestrator runs anyway and the watchdog owns
// that camera's recovery. The old all-or-nothing rollback turned a boot-time
// prime timeout into a permanently dead pipeline.
TEST(PipelineOrchestratorTest, StartToleratesCameraThatFailsToPrime) {
    // A capture that ignores Start() — IsRunning never flips true.
    class StuckCapture final : public ICaptureFrame {
       public:
        auto Start() -> void override {}
        auto Stop() -> void override {}
        [[nodiscard]] auto IsRunning() const -> bool override { return false; }
        auto Capture() -> std::optional<Frame> override {
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
            return std::nullopt;
        }
    };

    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<StuckCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    EXPECT_TRUE(orchestrator.Start());
    EXPECT_TRUE(orchestrator.IsRunning());
    orchestrator.Stop();
    EXPECT_FALSE(orchestrator.IsRunning());
}

// Start() still fails when there are no cameras at all (wiring bug, not a
// recoverable runtime condition).
TEST(PipelineOrchestratorTest, StartFailsWithNoCamerasConfigured) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        std::vector<CameraChain>{}, std::make_unique<FakePostprocessor>(),
        std::make_unique<StaticDecision>(), record_sink, sink, FastConfig());
    EXPECT_FALSE(orchestrator.Start());
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, EndToEndFlowProducesPostprocessedFrames) {
    auto preprocessor_owner = std::make_unique<FakePreprocessor>();
    auto postprocessor_owner = std::make_unique<FakePostprocessor>();
    auto* preprocessor = preprocessor_owner.get();
    auto* postprocessor = postprocessor_owner.get();
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::move(preprocessor_owner)),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), record_sink, sink,
        FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    // Run for ~300ms — at 33ms/frame in FakeCapture that's ~9 frames captured
    // with most or all reaching the sink (LatestOnlySlot may drop a couple).
    std::this_thread::sleep_for(std::chrono::milliseconds(
        300));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    EXPECT_GT(preprocessor->process_calls.load(), 0);
    EXPECT_GT(postprocessor->process_calls, 0);
    auto [pushes, last_id, last_format, last_geom] = sink.Snapshot();
    EXPECT_GT(pushes, 0);
    EXPECT_EQ(last_format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(last_geom.width, kOutputDims.width);
    EXPECT_EQ(last_geom.height, kOutputDims.height);
    // F6a: the recorder branch receives the same post-processed frames as the
    // stream branch (clean here — no overlay source wired in this test).
    EXPECT_GT(std::get<0>(record_sink.Snapshot()), 0);
}

// Watchdog: a capture pipeline that dies mid-run (Argus INVALID_SETTINGS on the
// WiFi-Direct radio reform → HandleBusMessages()->Stop()) must be restarted by
// the producer, and frames must resume reaching the sink. Without the watchdog
// the producer spins Capture()->nullopt on the dead pipeline forever (blank
// preview, 0-byte recording).
TEST(PipelineOrchestratorTest, WatchdogRestartsDeadCaptureAndFramesResume) {
    auto capture_owner = std::make_unique<FakeCapture>(kCam0Dims);
    auto* capture = capture_owner.get();
    capture->SetDieAfter(3);  // die once after 3 frames, then recover on Restart()

    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::move(capture_owner), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    // ~400ms: dies ~frame 3 (~100ms), watchdog Restart()s, frames resume for the
    // remainder — well past the 3-frame death point.
    std::this_thread::sleep_for(std::chrono::milliseconds(
        400));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    // Start() ran at least twice: once at orchestrator Start(), once via the
    // watchdog's Restart() after the death.
    EXPECT_GE(capture->StartCalls(), 2);
    // Frames resumed after recovery: more reached the sink than the 3 produced
    // before the pipeline died.
    auto [pushes, last_id, last_format, last_geom] = sink.Snapshot();
    EXPECT_GT(pushes, 3);
    EXPECT_GT(last_id, 3U);
}

// U3: both cameras run, but the static decision routes only camera 0 to
// postprocess. Camera 1 has a distinct geometry; if any cam-1 bundle reached
// postprocess the recorded source geometry would show 800x600.
TEST(PipelineOrchestratorTest, OnlyChosenCameraReachesPostprocess) {
    auto postprocessor_owner = std::make_unique<FakePostprocessor>();
    auto* postprocessor = postprocessor_owner.get();
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), record_sink, sink,
        FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(
        300));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    ASSERT_GT(postprocessor->process_calls, 0);
    // Every postprocessed frame came from camera 0 (640x360) — camera 1's
    // 800x600 bundles aged out unchosen.
    EXPECT_EQ(postprocessor->last_source_geometry.width, kCam0Dims.width);
    EXPECT_EQ(postprocessor->last_source_geometry.height, kCam0Dims.height);
    EXPECT_EQ(postprocessor->last_crop.width, kCam0Dims.width);
    EXPECT_EQ(postprocessor->last_crop.height, kCam0Dims.height);
}

// U3: if camera 1 stalls (produces no frames), the consumer still serves
// camera 0 — no deadlock, frames keep reaching the sink.
TEST(PipelineOrchestratorTest, OneCameraStallingDoesNotBlockTheOther) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims, /*stall=*/true),
                   std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(
        300));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    auto [pushes, last_id, last_format, last_geom] = sink.Snapshot();
    EXPECT_GT(pushes, 0) << "camera 0 should keep flowing while camera 1 stalls";
    EXPECT_FALSE(orchestrator.IsRunning());
}

// U3: Start/Stop must spawn and join both producer threads + the consumer
// cleanly, repeatedly.
TEST(PipelineOrchestratorTest, DualCameraStartStopCyclesCleanly) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(orchestrator.Start());
        EXPECT_TRUE(orchestrator.IsRunning());
        std::this_thread::sleep_for(std::chrono::milliseconds(
            60));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
        orchestrator.Stop();
        EXPECT_FALSE(orchestrator.IsRunning());
    }
}

// before any frame is produced, the snapshot source has nothing — the
// thumbnail handler turns this into a ResponseStatus::ERROR.
TEST(PipelineOrchestratorTest, GrabLatestIsEmptyBeforeAnyFrame) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    EXPECT_FALSE(orchestrator.GrabLatest().has_value());
}

// once the consumer has produced final frames, GrabLatest() returns the latest
// post-processed frame (BGR8, output geometry) — an owned snapshot the thumbnail
// path can encode after the call returns. Poll (instead of a fixed sleep) so the
// test isn't flaky under load.
TEST(PipelineOrchestratorTest, GrabLatestReturnsLatestFinalFrame) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());

    std::optional<Frame> snap;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        snap = orchestrator.GrabLatest();
        if (snap.has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            5));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    }
    orchestrator.Stop();

    if (!snap) {
        FAIL() << "no final frame produced within the poll window";
        return;
    }
    EXPECT_EQ(snap->format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(snap->geometry.width, kOutputDims.width);
    EXPECT_EQ(snap->geometry.height, kOutputDims.height);
}

// GrabLatest() racing Stop() must be safe: a reader thread hammering GrabLatest()
// while the main thread tears the pipeline down must neither crash nor deadlock
// (sanitizers catch any data race on the shared latest-frame slot).
TEST(PipelineOrchestratorTest, ConcurrentGrabLatestAndStopIsSafe) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());

    std::atomic<bool> stop_reader{false};
    std::atomic<int> grab_calls{0};
    std::thread reader([&] {
        while (!stop_reader.load(std::memory_order_relaxed)) {
            (void)orchestrator.GrabLatest();
            grab_calls.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let a few frames flow, then tear down while the reader is still grabbing.
    std::this_thread::sleep_for(std::chrono::milliseconds(
        60));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    // GrabLatest after Stop must remain safe (returns nullopt or a final frame).
    (void)orchestrator.GrabLatest();

    stop_reader.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_GT(grab_calls.load(), 0);
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, PostprocessorReceivesFullFrameCrop) {
    auto postprocessor_owner = std::make_unique<FakePostprocessor>();
    auto* postprocessor = postprocessor_owner.get();
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), record_sink, sink,
        FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(
        120));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    ASSERT_GT(postprocessor->process_calls, 0);
    // Static decision is "no AI yet, full-frame crop" — so the crop must exactly
    // cover the source frame.
    EXPECT_EQ(postprocessor->last_crop.x, 0U);
    EXPECT_EQ(postprocessor->last_crop.y, 0U);
    EXPECT_EQ(postprocessor->last_crop.width, postprocessor->last_source_geometry.width);
    EXPECT_EQ(postprocessor->last_crop.height, postprocessor->last_source_geometry.height);
}

TEST(PipelineOrchestratorTest, PreprocessRefusalsDoNotStallPipeline) {
    auto preprocessor_owner = std::make_unique<FakePreprocessor>();
    preprocessor_owner->refuse_after_n_ = 2;  // first 2 succeed, rest refuse
    auto* preprocessor = preprocessor_owner.get();
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::move(preprocessor_owner)),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(
        300));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    // Preprocessor was called many times even though most refused; the
    // producer kept going instead of stalling on the failure.
    EXPECT_GT(preprocessor->process_calls.load(), 2);
    auto [pushes, last_id, last_format, last_geom] = sink.Snapshot();
    EXPECT_GT(pushes, 0);
    EXPECT_LE(pushes, 2);  // only the first two preprocess outputs reach the sink
}

TEST(PipelineOrchestratorTest, StopWithoutStartIsHarmless) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    orchestrator.Stop();  // no-op
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, DestructorStopsRunningPipeline) {
    CountingSink record_sink;
    CountingSink sink;
    {
        PipelineOrchestrator orchestrator(
            OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
            std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
            sink, FastConfig());
        ASSERT_TRUE(orchestrator.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(
            120));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
        // No explicit Stop(); destructor must clean up cleanly.
    }
    // The test asserts we got here without hangs / data races caught by
    // sanitizers.
    SUCCEED();
}

// #6 F6d: with SIDE_BY_SIDE selected, the consumer postprocesses BOTH cameras
// and routes the composite to the stream sink, while the recorder still gets the
// clean single (cam0) frame.
TEST(PipelineOrchestratorTest, SideBySideCompositesBothCamerasToStream) {
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    // Poll for the composite to flow rather than assuming a fixed sleep suffices —
    // a 300ms sleep flaked under the slower qemu CI emulation. Both cameras must
    // capture + postprocess + composite, so wait for a FRESH dual-pane composite
    // (right pane = postprocessed cam1) to land in the sink — the first ticks may
    // legitimately composite with an empty right pane before cam1's first frame
    // arrives (U4 re-composition).
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           (compositor.LastRightGeometry().width != kOutputDims.width ||
            std::get<3>(sink.Snapshot()).width != kCompositeDims.width)) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    EXPECT_GT(compositor.Calls(), 0);
    // The other camera (cam1, 800x600) was postprocessed and handed to the
    // compositor as the right input — proves cam1 is no longer aged out.
    EXPECT_EQ(compositor.LastRightGeometry().width, kOutputDims.width);
    // The stream sink received the composite geometry, not the single 1280x720.
    EXPECT_EQ(std::get<3>(sink.Snapshot()).width, kCompositeDims.width);
    // The recorder still got the clean single (postprocessed cam0) frame.
    EXPECT_EQ(std::get<3>(record_sink.Snapshot()).width, kOutputDims.width);
}

// Cadence follows the selection: with camera 1 selected, the consumer blocks on
// camera 1, so the stream keeps flowing camera-1 frames even when camera 0 is
// fully stalled (dead). Under the old hardcoded cam0 cadence a stalled cam0 would
// pace the whole loop off its pop-timeout regardless of the selection.
TEST(PipelineOrchestratorTest, CadenceFollowsSelectionSoSelectedCameraStreamsWhenOtherStalls) {
    constexpr std::uint64_t kCam1IdBase = 1000;
    ManualCameraState camera_state;
    camera_state.Set(1);  // select camera 1
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims, /*stall=*/true, 0),
                   std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims, /*stall=*/false, kCam1IdBase),
                   std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<ManualDecision>(camera_state),
        record_sink, sink, FastConfig());

    ASSERT_TRUE(orchestrator.Start());
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline && std::get<0>(sink.Snapshot()) == 0) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    const auto [pushes, last_id, format, geom] = sink.Snapshot();
    EXPECT_GT(pushes, 0);             // stream flowed even though camera 0 is dead
    EXPECT_GE(last_id, kCam1IdBase);  // and the streamed frames are camera 1's
}

// Side-by-side pane order is POSITIONAL: camera 0 is always the left pane, even
// when camera 1 is the selected main feed. Changing the selection must not swap
// the panes. cam1 uses a high frame_id base so a composite whose left pane came
// from cam0 is provable by id.
TEST(PipelineOrchestratorTest, SideBySideKeepsCameraZeroLeftWhenCameraOneSelected) {
    constexpr std::uint64_t kCam1IdBase = 1000;
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    ManualCameraState camera_state;
    camera_state.Set(1);  // select camera 1 as the main feed
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims, false, 0),
                   std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims, false, kCam1IdBase),
                   std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<ManualDecision>(camera_state),
        record_sink, sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (
        std::chrono::steady_clock::now() < deadline &&
        (compositor.Calls() == 0 || std::get<3>(sink.Snapshot()).width != kCompositeDims.width)) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    ASSERT_GT(compositor.Calls(), 0);
    // Left pane frame_id is below cam1's base → it came from camera 0, even
    // though camera 1 is the selection. Positional order held.
    EXPECT_LT(compositor.LastLeftId(), kCam1IdBase);
    EXPECT_EQ(std::get<3>(sink.Snapshot()).width, kCompositeDims.width);
}

// ── U4: both-mode never degrades to a bare single-camera frame ───────
//
// The reported flicker: in side-by-side mode a per-tick miss of the non-cadence
// camera (or a composite failure) used to fall back to the full single-camera
// frame — an occasional full-frame flash in the both-mode preview. U4 semantics:
// short misses repeat the previous composite; past the hold cap the live pane
// keeps advancing per tick while the stale pane holds its last-good frame; the
// stream sink NEVER carries a bare chosen-camera frame while layout=both.

// Poll until a FRESH dual-pane composite (right pane = postprocessed cam1,
// kOutputDims) has flowed to the sink, so a test starts from a known-good
// composite before injecting its failure.
auto WaitForFreshComposite(const FakeCompositor& compositor, const CountingSink& sink) -> bool {
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (compositor.LastRightGeometry().width == kOutputDims.width &&
            std::get<3>(sink.Snapshot()).width == kCompositeDims.width) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

// U4 edge: the other camera misses a few ticks (short stall, within the hold
// cap) → the previous composite is repeated, the stream keeps flowing, and no
// push ever equals the bare chosen-camera frame.
TEST(PipelineOrchestratorTest, SideBySideOtherCameraMissHoldsCompositeNeverBareSingle) {
    auto cam1_owner = std::make_unique<FakeCapture>(kCam1Dims);
    auto* cam1 = cam1_owner.get();
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::move(cam1_owner), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForFreshComposite(compositor, sink));

    // Stall the non-cadence camera for a handful of consumer ticks.
    cam1->SetStall(true);
    const int pushes_at_stall = sink.PushCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(
        150));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    // The stream kept flowing during the stall (held composite repeated)…
    EXPECT_GT(sink.PushCount(), pushes_at_stall);
    // …and NOTHING that reached the sink was ever the bare chosen frame: every
    // push in both-mode carried the composite geometry.
    EXPECT_EQ(sink.CountPushesWithWidth(kOutputDims.width), 0);
    EXPECT_EQ(sink.CountPushesWithWidth(kCompositeDims.width), sink.PushCount());
}

// U4 edge: the other camera stalls PAST the hold cap → per-tick re-composition:
// the live chosen pane keeps advancing across ticks while the stale pane holds
// the stalled camera's last-good frame. A sustained one-camera stall (a full
// RECOVERING restart window) must not freeze the healthy pane.
TEST(PipelineOrchestratorTest, SideBySideSustainedStallAdvancesLivePaneAndHoldsStalePane) {
    constexpr std::uint64_t kCam1IdBase = 1000;
    auto cam1_owner = std::make_unique<FakeCapture>(kCam1Dims, /*stall=*/false, kCam1IdBase);
    auto* cam1 = cam1_owner.get();
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::move(cam1_owner), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForFreshComposite(compositor, sink));

    cam1->SetStall(true);
    const int calls_at_stall = compositor.Calls();
    // Wait until re-composition is clearly running: several compositor calls
    // AFTER the stall (the first ~hold-cap miss ticks repeat the held composite
    // without calling the compositor, then re-composition calls it every tick).
    constexpr int kRecompositions = 4;
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           compositor.Calls() < calls_at_stall + kRecompositions) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    const auto records = compositor.Records();
    ASSERT_GE(records.size(), 2U);
    const auto& previous = records[records.size() - 2];
    const auto& last = records[records.size() - 1];
    // Live pane advanced across ticks (chosen camera never froze)…
    EXPECT_GT(last.left_id, previous.left_id);
    // …while the stale pane held the SAME last-good cam1 frame (not black, not
    // advancing).
    EXPECT_EQ(last.right_id, previous.right_id);
    EXPECT_GE(last.right_id, kCam1IdBase);
    // And still zero bare single-camera frames on the stream.
    EXPECT_EQ(sink.CountPushesWithWidth(kOutputDims.width), 0);
}

// U4 edge: CompositeSideBySide failing (nullopt) follows the same hold
// semantics — the previous composite is repeated, never the bare chosen frame.
TEST(PipelineOrchestratorTest, SideBySideCompositeFailureHoldsPreviousComposite) {
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForFreshComposite(compositor, sink));

    compositor.SetFail(true);
    const int pushes_at_failure = sink.PushCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(
        250));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
    orchestrator.Stop();

    // The stream kept flowing on the held composite despite every composite
    // attempt failing…
    EXPECT_GT(sink.PushCount(), pushes_at_failure);
    // …and no bare single-camera frame ever reached the sink.
    EXPECT_EQ(sink.CountPushesWithWidth(kOutputDims.width), 0);
    EXPECT_EQ(sink.CountPushesWithWidth(kCompositeDims.width), sink.PushCount());
}

// U4 edge: an intentional layout switch to SINGLE drops the held state — single
// frames flow immediately, and a later return to both-mode starts fresh (the
// previously-held cam1 pane is gone: re-composition runs with an empty pane, not
// a resurrected stale one).
TEST(PipelineOrchestratorTest, SideBySideLayoutSwitchToSingleDropsHeldState) {
    auto cam1_owner = std::make_unique<FakeCapture>(kCam1Dims);
    auto* cam1 = cam1_owner.get();
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::move(cam1_owner), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    ASSERT_TRUE(WaitForFreshComposite(compositor, sink));

    // Stall cam1 so the hold is actively bridging when the layout switches.
    cam1->SetStall(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(
        100));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration

    // Switch to SINGLE: single (kOutputDims) frames must flow immediately.
    layout.Set(sst::streaming::PreviewLayout::kSingle);
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           std::get<3>(sink.Snapshot()).width != kOutputDims.width) {
        std::this_thread::sleep_for(kPollInterval);
    }
    EXPECT_EQ(std::get<3>(sink.Snapshot()).width, kOutputDims.width);

    // Switch back to both-mode with cam1 still stalled: the held pane was
    // dropped, so re-composition runs with an EMPTY stale pane (black), not the
    // pre-switch cam1 frame.
    const auto records_before_return = compositor.Records().size();
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           compositor.Records().size() <= records_before_return) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    const auto records = compositor.Records();
    ASSERT_GT(records.size(), records_before_return);
    // Every post-return composite got the dropped-state empty pane: zero width,
    // frame_id 0 — nothing held survived the layout round-trip.
    for (std::size_t i = records_before_return; i < records.size(); ++i) {
        EXPECT_EQ(records[i].right_geometry.width, 0U);
        EXPECT_EQ(records[i].right_id, 0U);
    }
}

// U4 integration: the other camera producing SLOWER than the consumer cadence
// (alternating hit/miss at consumer rate — the exact phase pattern behind the
// reported flicker) yields a stable all-composite stream: zero single-frame
// interleaves.
TEST(PipelineOrchestratorTest, SideBySideAlternatingMissesProduceStableCompositeStream) {
    auto cam1_owner = std::make_unique<FakeCapture>(kCam1Dims);
    // ~2.5x slower than cam0: the non-cadence TryPop misses on most ticks and
    // hits on the rest, interleaving fresh composites with held repeats.
    cam1_owner->SetFramePeriod(std::chrono::milliseconds(
        80));  // NOLINT(readability-magic-numbers) — self-evident pacing choice
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;
    layout.Set(sst::streaming::PreviewLayout::kSideBySide);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::move(cam1_owner), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    // Let a meaningful stretch of alternating hits/misses flow.
    constexpr int kMinPushes = 12;
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline && sink.PushCount() < kMinPushes) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    EXPECT_GE(sink.PushCount(), kMinPushes);
    // A stable composite stream: EVERY push carried the composite geometry —
    // zero bare single-camera interleaves (the flicker).
    EXPECT_EQ(sink.CountPushesWithWidth(kOutputDims.width), 0);
    EXPECT_EQ(sink.CountPushesWithWidth(kCompositeDims.width), sink.PushCount());
}

// #6 F6d: SINGLE (default) never invokes the compositor; the stream carries the
// single postprocessed frame even when a compositor is wired.
TEST(PipelineOrchestratorTest, SingleLayoutSkipsCompositor) {
    FakeCompositor compositor;
    sst::streaming::PreviewLayoutState layout;  // defaults to kSingle
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig(), nullptr, nullptr, &compositor, &layout);

    ASSERT_TRUE(orchestrator.Start());
    // Poll for a frame to reach the sink (qemu is slow; a fixed sleep flakes).
    constexpr auto kFlowTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kFlowTimeout;
    while (std::chrono::steady_clock::now() < deadline &&
           std::get<3>(sink.Snapshot()).width != kOutputDims.width) {
        std::this_thread::sleep_for(kPollInterval);
    }
    orchestrator.Stop();

    EXPECT_EQ(compositor.Calls(), 0);
    EXPECT_EQ(std::get<3>(sink.Snapshot()).width, kOutputDims.width);
}

// U6 autofocus tap: GrabCameraFrame returns the requested camera's SOURCE
// frame (per-camera geometry proves which camera served it), not the chosen/
// postprocessed output — including for the camera the decision never selects.
TEST(PipelineOrchestratorTest, GrabCameraFrameTapsTheRequestedCamerasProducerPath) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    ASSERT_TRUE(orchestrator.Start());

    // Generous timeout: qemu is slow and FakeCapture paces at ~30fps.
    constexpr auto kGrabTimeout = std::chrono::seconds(5);
    const auto cam0 = orchestrator.GrabCameraFrame(0, kGrabTimeout);
    const auto cam1 = orchestrator.GrabCameraFrame(1, kGrabTimeout);  // never the chosen camera
    orchestrator.Stop();

    ASSERT_TRUE(cam0.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(cam0->geometry.width, kCam0Dims.width);
    ASSERT_TRUE(cam1.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(cam1->geometry.width, kCam1Dims.width);
}

// The tap is shared by TWO samplers on metal (the AF loop and the auto-color
// loop). The publication is a broadcast: concurrent grabs on the SAME camera
// must BOTH be served — the original single-sample hand-off let one waiter
// steal the other's sample, starving whichever loop kept losing the race.
TEST(PipelineOrchestratorTest, ConcurrentGrabsOnOneCameraAreBothServed) {
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    ASSERT_TRUE(orchestrator.Start());

    constexpr auto kGrabTimeout = std::chrono::seconds(5);  // qemu headroom
    constexpr int kRounds = 5;
    int hits_a = 0;
    int hits_b = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::optional<sst::capture::Frame> got_a;
        std::thread rival([&] { got_a = orchestrator.GrabCameraFrame(0, kGrabTimeout); });
        if (orchestrator.GrabCameraFrame(0, kGrabTimeout).has_value()) {
            ++hits_b;
        }
        rival.join();
        if (got_a.has_value()) {
            ++hits_a;
        }
    }
    orchestrator.Stop();

    // Every round serves BOTH grabbers — no starvation, no stolen samples.
    EXPECT_EQ(hits_a, kRounds);
    EXPECT_EQ(hits_b, kRounds);
}

// U6 autofocus tap edges: an out-of-range camera and a stalled camera both
// yield nullopt (the latter after the bounded wait) — the AF loop must never
// block unbounded on a camera that produces nothing.
TEST(PipelineOrchestratorTest, GrabCameraFrameTimesOutOnStalledCamera) {
    auto capture = std::make_unique<FakeCapture>(kCam0Dims, /*stall=*/true);
    CountingSink record_sink;
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::move(capture), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), record_sink,
        sink, FastConfig());
    ASSERT_TRUE(orchestrator.Start());

    constexpr auto kShortTimeout = std::chrono::milliseconds(50);
    constexpr std::size_t kOutOfRangeCamera = 7;
    EXPECT_FALSE(orchestrator.GrabCameraFrame(0, kShortTimeout).has_value());  // stalled
    EXPECT_FALSE(orchestrator.GrabCameraFrame(kOutOfRangeCamera, kShortTimeout)
                     .has_value());  // out of range
    orchestrator.Stop();
    EXPECT_FALSE(orchestrator.GrabCameraFrame(0, kShortTimeout).has_value());  // stopped
}

}  // namespace
