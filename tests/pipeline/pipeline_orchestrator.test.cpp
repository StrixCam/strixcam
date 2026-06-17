#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/capture/ports/frame-src.hpp"
#include "app/decision/services/static_decision/static-decision.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/processing/ports/postprocessor.hpp"
#include "app/processing/ports/preprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace {

using sst::buffer::IFrameSink;
using sst::capture::Frame;
using sst::capture::ICaptureFrame;
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
    explicit FakeCapture(Dims dims, bool stall = false)
        : width_(dims.width), height_(dims.height), stall_(stall) {}

    auto Start() -> void override { running_ = true; }
    auto Stop() -> void override { running_ = false; }
    [[nodiscard]] auto IsRunning() const -> bool override { return running_; }

    auto Capture() -> std::optional<Frame> override {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(kFramePeriodMs));
        return frame;
    }

   private:
    std::uint32_t width_{kCam0Dims.width};
    std::uint32_t height_{kCam0Dims.height};
    bool stall_{false};
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
    auto Process(const Frame& source, const CropRect& crop) -> std::optional<Frame> override {
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
    }

    auto Snapshot() const
        -> std::tuple<int, std::uint64_t, sst::common::PixelFormat, sst::capture::FrameGeometry> {
        std::lock_guard lock(mtx_);
        return {push_calls, last_frame_id, last_format, last_geometry};
    }

   private:
    mutable std::mutex mtx_;
    int push_calls{0};
    std::uint64_t last_frame_id{0};
    sst::common::PixelFormat last_format{};
    sst::capture::FrameGeometry last_geometry{};
};

// ── Construction helpers ─────────────────────────────────────────────
//
// The orchestrator is non-movable, so we build the camera-chain vector with
// these helpers and pass it straight into the constructor. Single-camera tests
// use OneCamera + a real StaticDecision (deterministic: cam 0 full-frame).

auto OneCamera(std::unique_ptr<ICaptureFrame> capture, std::unique_ptr<IPreprocessor> preprocessor)
    -> std::vector<CameraChain> {
    std::vector<CameraChain> chains;
    chains.push_back(
        CameraChain{.capture = std::move(capture), .preprocessor = std::move(preprocessor)});
    return chains;
}

auto TwoCameras(std::unique_ptr<ICaptureFrame> cam0, std::unique_ptr<IPreprocessor> pre0,
                std::unique_ptr<ICaptureFrame> cam1, std::unique_ptr<IPreprocessor> pre1)
    -> std::vector<CameraChain> {
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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

    EXPECT_FALSE(orchestrator.IsRunning());
    EXPECT_TRUE(orchestrator.Start());
    EXPECT_TRUE(orchestrator.IsRunning());

    EXPECT_TRUE(orchestrator.Start());  // idempotent
    EXPECT_TRUE(orchestrator.IsRunning());

    orchestrator.Stop();
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, FailsToStartWhenCaptureFailsToStart) {
    // A capture that ignores Start() — IsRunning never flips true.
    class StuckCapture final : public ICaptureFrame {
       public:
        auto Start() -> void override {}
        auto Stop() -> void override {}
        [[nodiscard]] auto IsRunning() const -> bool override { return false; }
        auto Capture() -> std::optional<Frame> override { return std::nullopt; }
    };

    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<StuckCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());
    EXPECT_FALSE(orchestrator.Start());
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, EndToEndFlowProducesPostprocessedFrames) {
    auto preprocessor_owner = std::make_unique<FakePreprocessor>();
    auto postprocessor_owner = std::make_unique<FakePostprocessor>();
    auto* preprocessor = preprocessor_owner.get();
    auto* postprocessor = postprocessor_owner.get();
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::move(preprocessor_owner)),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), sink, FastConfig());

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
}

// U3: both cameras run, but the static decision routes only camera 0 to
// postprocess. Camera 1 has a distinct geometry; if any cam-1 bundle reached
// postprocess the recorded source geometry would show 800x600.
TEST(PipelineOrchestratorTest, OnlyChosenCameraReachesPostprocess) {
    auto postprocessor_owner = std::make_unique<FakePostprocessor>();
    auto* postprocessor = postprocessor_owner.get();
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), sink, FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims, /*stall=*/true),
                   std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        TwoCameras(std::make_unique<FakeCapture>(kCam0Dims), std::make_unique<FakePreprocessor>(),
                   std::make_unique<FakeCapture>(kCam1Dims), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());
    EXPECT_FALSE(orchestrator.GrabLatest().has_value());
}

// once the consumer has produced final frames, GrabLatest() returns the latest
// post-processed frame (BGR8, output geometry) — an owned snapshot the thumbnail
// path can encode after the call returns. Poll (instead of a fixed sleep) so the
// test isn't flaky under load.
TEST(PipelineOrchestratorTest, GrabLatestReturnsLatestFinalFrame) {
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

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

    ASSERT_TRUE(snap.has_value()) << "no final frame produced within the poll window";
    EXPECT_EQ(snap->format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(snap->geometry.width, kOutputDims.width);
    EXPECT_EQ(snap->geometry.height, kOutputDims.height);
}

// GrabLatest() racing Stop() must be safe: a reader thread hammering GrabLatest()
// while the main thread tears the pipeline down must neither crash nor deadlock
// (sanitizers catch any data race on the shared latest-frame slot).
TEST(PipelineOrchestratorTest, ConcurrentGrabLatestAndStopIsSafe) {
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::move(postprocessor_owner), std::make_unique<StaticDecision>(), sink, FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::move(preprocessor_owner)),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());

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
    CountingSink sink;
    PipelineOrchestrator orchestrator(
        OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
        std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
        FastConfig());
    orchestrator.Stop();  // no-op
    EXPECT_FALSE(orchestrator.IsRunning());
}

TEST(PipelineOrchestratorTest, DestructorStopsRunningPipeline) {
    CountingSink sink;
    {
        PipelineOrchestrator orchestrator(
            OneCamera(std::make_unique<FakeCapture>(), std::make_unique<FakePreprocessor>()),
            std::make_unique<FakePostprocessor>(), std::make_unique<StaticDecision>(), sink,
            FastConfig());
        ASSERT_TRUE(orchestrator.Start());
        std::this_thread::sleep_for(std::chrono::milliseconds(
            120));  // NOLINT(readability-magic-numbers) — self-evident gtest run/poll duration
        // No explicit Stop(); destructor must clean up cleanly.
    }
    // The test asserts we got here without hangs / data races caught by
    // sanitizers.
    SUCCEED();
}

}  // namespace
