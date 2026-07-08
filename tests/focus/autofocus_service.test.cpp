#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/camera-focus.handler.hpp"
#include "app/focus/ports/focuser.hpp"
#include "app/focus/services/autofocus/autofocus-service.hpp"
#include "app/pipeline/ports/camera-frame-tap.hpp"
#include "bluetooth.pb.h"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/focus/models/focus-state.hpp"
#include "domain/health/models/camera-health.hpp"

namespace {

using sst::capture::Frame;
using sst::capture::FramePlane;
using sst::focus::AutofocusConfig;
using sst::focus::AutofocusService;
using sst::focus::FocusState;
using sst::health::CameraHealth;

// NOLINTBEGIN(readability-magic-numbers) — named test-scene definitions
constexpr std::uint32_t kFrameSide = 64;
constexpr std::uint8_t kMidGrey = 128;
constexpr double kPeakAmplitude = 100.0;
// Peaks chosen so the coarse lattice (step 80) has a unique best sample and
// the fine lattice (step 20) hits the peak exactly.
constexpr std::uint32_t kCam0Peak = 640;
constexpr std::uint32_t kMovedPeak = 200;
constexpr std::uint32_t kCam1Peak = 400;
constexpr std::uint32_t kManualPosition = 500;
// NOLINTEND(readability-magic-numbers)

// Fast loop for tests; generous grab timeout because qemu runs slow.
auto FastConfig() -> AutofocusConfig {
    constexpr auto kTestTick = std::chrono::milliseconds(2);
    constexpr auto kTestSampleTimeout = std::chrono::milliseconds(100);
    constexpr auto kTestBackoff = std::chrono::milliseconds(40);
    AutofocusConfig config;
    config.tick_interval = kTestTick;
    config.sample_timeout = kTestSampleTimeout;
    config.i2c_backoff = kTestBackoff;
    return config;
}

constexpr auto kSettleWindow = std::chrono::milliseconds(120);  // "no writes" observation span
constexpr auto kDeadline = std::chrono::seconds(10);            // per-wait cap (qemu headroom)
constexpr auto kPollInterval = std::chrono::milliseconds(5);

// Thread-safe focuser double: records every attempted write; the "lens"
// position only moves on a successful write.
class RecordingFocuser final : public sst::focus::IFocuser {
   public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) floor-ok: test-double config flags
    explicit RecordingFocuser(bool available = true, bool write_succeeds = true)
        : available_(available), write_succeeds_(write_succeeds) {}

    auto SetFocus(std::uint32_t camera_index, std::uint32_t position) -> bool override {
        const std::lock_guard lock(mtx_);
        writes_.emplace_back(camera_index, position);
        if (write_succeeds_ && camera_index < positions_.size()) {
            positions_[camera_index] = position;
        }
        return write_succeeds_;
    }
    [[nodiscard]] auto IsAvailable() const -> bool override { return available_; }

    [[nodiscard]] auto Position(std::uint32_t camera_index) const -> std::uint32_t {
        const std::lock_guard lock(mtx_);
        return camera_index < positions_.size() ? positions_[camera_index] : 0;
    }
    [[nodiscard]] auto WriteCount() const -> std::size_t {
        const std::lock_guard lock(mtx_);
        return writes_.size();
    }
    [[nodiscard]] auto LastWrite() const -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        const std::lock_guard lock(mtx_);
        return writes_.empty() ? std::nullopt : std::optional{writes_.back()};
    }
    [[nodiscard]] auto WritesFor(std::uint32_t camera_index) const -> std::size_t {
        const std::lock_guard lock(mtx_);
        std::size_t count = 0;
        for (const auto& [camera, position] : writes_) {
            count += camera == camera_index ? 1 : 0;
        }
        return count;
    }

   private:
    mutable std::mutex mtx_;
    bool available_;
    bool write_succeeds_;
    std::array<std::uint32_t, FocusState::kCameras> positions_{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes_;
};

// Synthetic scene: each grabbed frame is a checkerboard whose contrast peaks
// when the fake lens sits at that camera's peak position — Laplacian variance
// then falls off strictly with |position - peak|, exactly what a defocusing
// lens does. Optional gating hands the loop samples one-by-one (lock-step,
// for the deterministic mid-sweep preemption test); optional noise wobbles
// contrast a few percent per sample (plateau-noise test).
class SyntheticSceneTap final : public sst::pipeline::ICameraFrameTap {
   public:
    explicit SyntheticSceneTap(const RecordingFocuser& focuser, bool gated = false)
        : focuser_(focuser), gated_(gated) {}

    auto SetPeak(std::size_t camera_index, std::uint32_t peak) -> void {
        const std::lock_guard lock(mtx_);
        peaks_[camera_index] = peak;
    }
    auto SetNoisy(bool noisy) -> void {
        const std::lock_guard lock(mtx_);
        noisy_ = noisy;
    }
    auto Release(int samples) -> void {
        {
            const std::lock_guard lock(mtx_);
            released_ += samples;
        }
        cv_.notify_all();
    }
    [[nodiscard]] auto GrabCount() const -> int {
        const std::lock_guard lock(mtx_);
        return grabs_;
    }

    [[nodiscard]] auto GrabCameraFrame(std::size_t camera_index, std::chrono::milliseconds timeout)
        -> std::optional<Frame> override {
        std::uint32_t peak = 0;
        double wobble = 1.0;
        {
            std::unique_lock lock(mtx_);
            ++grabs_;
            if (gated_) {
                if (!cv_.wait_for(lock, timeout, [this] { return consumed_ < released_; })) {
                    return std::nullopt;
                }
                ++consumed_;
            }
            peak = camera_index < peaks_.size() ? peaks_[camera_index] : 0;
            if (noisy_) {
                constexpr double kNoiseLow = 0.94;
                constexpr double kNoiseHigh = 1.06;
                wobble = (sequence_++ % 2) == 0 ? kNoiseLow : kNoiseHigh;
            }
        }
        const auto position = focuser_.Position(static_cast<std::uint32_t>(camera_index));
        const double distance = std::abs(static_cast<double>(position) - static_cast<double>(peak));
        const double amplitude =
            kPeakAmplitude * (1.0 - distance / static_cast<double>(sst::focus::kMaxFocus)) * wobble;
        return MakeCheckerboardFrame(static_cast<std::uint8_t>(std::lround(amplitude)));
    }

   private:
    static auto MakeCheckerboardFrame(std::uint8_t amplitude) -> Frame {
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(kFrameSide) * kFrameSide, kMidGrey);
        for (std::uint32_t py = 0; py < kFrameSide; ++py) {
            for (std::uint32_t px = 0; px < kFrameSide; ++px) {
                const bool white = ((px + py) % 2) == 0;
                (*pixels)[static_cast<std::size_t>(py) * kFrameSide + px] =
                    white ? static_cast<std::uint8_t>(kMidGrey + amplitude)
                          : static_cast<std::uint8_t>(kMidGrey - amplitude);
            }
        }
        Frame frame;
        frame.format = sst::common::PixelFormat::GRAY8;
        frame.memory = sst::common::MemoryType::CPU;
        frame.geometry = {.width = kFrameSide, .height = kFrameSide};
        frame.planes.push_back(
            FramePlane{.stride = kFrameSide, .data = pixels->data(), .size = pixels->size()});
        frame.owner = pixels;
        return frame;
    }

    const RecordingFocuser& focuser_;
    const bool gated_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::array<std::uint32_t, FocusState::kCameras> peaks_{};
    bool noisy_{false};
    int released_{0};
    int consumed_{0};
    int grabs_{0};
    int sequence_{0};
};

// Wait until the focuser write log has been stable for kSettleWindow (the loop
// converged / stood down), returning the settled count. Fails the deadline by
// returning the current count anyway — asserts downstream catch it.
auto WaitForWriteQuiescence(const RecordingFocuser& focuser) -> std::size_t {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    std::size_t last = focuser.WriteCount();
    auto stable_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPollInterval);
        const std::size_t now_count = focuser.WriteCount();
        if (now_count != last) {
            last = now_count;
            stable_since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - stable_since >= kSettleWindow) {
            break;
        }
    }
    return last;
}

auto WaitForWriteCountAtLeast(const RecordingFocuser& focuser, std::size_t minimum) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        if (focuser.WriteCount() >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

TEST(AutofocusServiceTest, BootDefaultIsAutoForAllCameras) {
    // Continuous AF is on from boot (matches the proto FOCUS_MODE_AUTO
    // default) — no app command needed to start the loop.
    const FocusState state;
    EXPECT_TRUE(state.IsAuto(0));
    EXPECT_TRUE(state.IsAuto(1));
}

TEST(AutofocusServiceTest, ConvergesToSharpnessPeakAndHolds) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);  // isolate camera 0 (boot default is AUTO on both)
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    const std::size_t settled = WaitForWriteQuiescence(focuser);
    ASSERT_GT(settled, 0U);
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->first, 0U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kCam0Peak);  // converged exactly on the peak

    // Holding: sharpness is watched but the lens is NOT touched — zero further
    // writes over a long observation window (no oscillation on a still scene).
    std::this_thread::sleep_for(kSettleWindow);
    EXPECT_EQ(focuser.WriteCount(), settled);
    // The manually-flipped camera was never hunted.
    EXPECT_EQ(focuser.WritesFor(1), 0U);
    service.Stop();
}

TEST(AutofocusServiceTest, PlateauNoiseDoesNotRetriggerOrOscillate) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    const std::size_t settled = WaitForWriteQuiescence(focuser);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    ASSERT_EQ(focuser.LastWrite()->second, kCam0Peak);

    // A few percent of per-sample contrast wobble is plateau noise, far above
    // the retrigger hysteresis floor — the hold must absorb it silently.
    tap.SetNoisy(true);
    std::this_thread::sleep_for(kSettleWindow * 3);
    EXPECT_EQ(focuser.WriteCount(), settled);
    service.Stop();
}

TEST(AutofocusServiceTest, SustainedSharpnessDropRetriggersAndReconverges) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    WaitForWriteQuiescence(focuser);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    ASSERT_EQ(focuser.LastWrite()->second, kCam0Peak);

    // The scene's focal plane moves: sharpness at the held position collapses.
    // After the hysteresis streak the loop re-sweeps and converges on the new
    // peak.
    tap.SetPeak(0, kMovedPeak);
    WaitForWriteQuiescence(focuser);
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kMovedPeak);
    service.Stop();
}

TEST(AutofocusServiceTest, ManualCommandMidSweepAbortsSweepAndManualHolds) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser, /*gated=*/true);  // lock-step: we hand out samples
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);  // isolate camera 0
    sst::control::CameraFocusHandler handler(focuser, state);
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    // Let the sweep advance a few positions, then starve it: with no released
    // sample the loop grabs nothing and therefore writes nothing, so the
    // manual command below cannot race an in-flight sweep write.
    tap.Release(4);
    ASSERT_TRUE(WaitForWriteCountAtLeast(focuser, 3));
    WaitForWriteQuiescence(focuser);

    // Manual preemption through the REAL handler: mode flips to manual and the
    // handler itself drives the VCM to the manual position.
    sst_cam::Command cmd;
    auto* payload = cmd.mutable_camera_focus();
    payload->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_MANUAL);
    payload->set_focus_position(kManualPosition);
    payload->set_camera_index(0);
    const auto resp = handler.Handle(cmd);
    EXPECT_EQ(resp.camera_focus().mode(), sst_cam::CameraFocusMode::FOCUS_MODE_MANUAL);
    const std::size_t at_manual = focuser.WriteCount();

    // Feed the loop plenty of samples: the aborted sweep must never write
    // again — the manual position is the last word.
    constexpr int kFloodSamples = 50;
    tap.Release(kFloodSamples);
    std::this_thread::sleep_for(kSettleWindow * 2);
    EXPECT_EQ(focuser.WriteCount(), at_manual);
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kManualPosition);
    EXPECT_EQ(focuser.Position(0), kManualPosition);
    EXPECT_FALSE(state.IsAuto(0));
    service.Stop();
}

TEST(AutofocusServiceTest, RecordingDoesNotPauseTheLoopByDefault) {
    // Guard against ambient env leakage into this default-config test.
    ::unsetenv("SST_AF_DISABLE_DURING_RECORDING");
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);
    std::atomic<bool> recording{true};  // recording the whole time
    AutofocusService service(
        focuser, state, tap, nullptr, [&recording] { return recording.load(); }, FastConfig());
    service.Start();

    // Default: AF stays active through record/stream — hunts and converges
    // while the recording runs.
    WaitForWriteQuiescence(focuser);
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kCam0Peak);  // hunted and converged mid-recording
    service.Stop();
}

TEST(AutofocusServiceTest, EnvFlagPausesAutofocusDuringRecordingAndStoppingResumesIt) {
    ::setenv("SST_AF_DISABLE_DURING_RECORDING", "1", /*overwrite=*/1);
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);
    std::atomic<bool> recording{true};
    AutofocusService service(
        focuser, state, tap, nullptr, [&recording] { return recording.load(); }, FastConfig());
    ::unsetenv("SST_AF_DISABLE_DURING_RECORDING");  // captured at construction
    service.Start();

    // Rollback hatch: recording active from the start → the loop must not
    // hunt at all (the pre-flip R15 behavior).
    std::this_thread::sleep_for(kSettleWindow * 2);
    EXPECT_EQ(focuser.WriteCount(), 0U);

    // Recording stops → the loop resumes and converges.
    recording.store(false);
    WaitForWriteQuiescence(focuser);
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kCam0Peak);

    // Recording starts again mid-hold: the hold keeps its position (no writes
    // either way — but a re-trigger while paused must also stay silent).
    recording.store(true);
    tap.SetPeak(0, kMovedPeak);  // sharpness collapses at the held position...
    const std::size_t held = focuser.WriteCount();
    std::this_thread::sleep_for(kSettleWindow * 2);
    EXPECT_EQ(focuser.WriteCount(), held);  // ...but the env flag gates any re-sweep
    service.Stop();
}

TEST(AutofocusServiceTest, UnhealthyCameraIsNeverHunted) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    tap.SetPeak(1, kCam1Peak);
    FocusState state;
    state.SetAuto(0, true);
    state.SetAuto(1, true);
    // Camera 0 is stalled (U3 frame truth): hunting it would be I2C churn.
    const auto health = [](std::size_t camera_index) -> std::optional<CameraHealth> {
        return camera_index == 0 ? CameraHealth::kRecovering : CameraHealth::kOk;
    };
    AutofocusService service(focuser, state, tap, health, nullptr, FastConfig());
    service.Start();

    WaitForWriteQuiescence(focuser);
    EXPECT_EQ(focuser.WritesFor(0), 0U);  // unhealthy camera untouched
    const auto last = focuser.LastWrite();
    ASSERT_TRUE(last.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->first, 1U);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT/guard above
    EXPECT_EQ(last->second, kCam1Peak);  // healthy camera converged normally
    service.Stop();
}

TEST(AutofocusServiceTest, I2cWriteFailureBacksOffWithoutCrashing) {
    RecordingFocuser focuser(/*available=*/true, /*write_succeeds=*/false);
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);  // isolate camera 0's backoff cadence
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    // Every write fails: the loop must retry on the backoff cadence, not every
    // tick. Observe a window many ticks long (2ms tick, 40ms backoff): without
    // backoff it would attempt ~100+ writes; with it, a small handful.
    constexpr auto kObservation = std::chrono::milliseconds(300);
    constexpr std::size_t kMaxBackedOffAttempts = 15;
    std::this_thread::sleep_for(kObservation);
    const std::size_t attempts = focuser.WriteCount();
    EXPECT_GT(attempts, 0U);
    EXPECT_LT(attempts, kMaxBackedOffAttempts);
    service.Stop();  // clean shutdown — no crash, no hang
}

TEST(AutofocusServiceTest, FixedLensLoopIdlesEntirely) {
    RecordingFocuser focuser(/*available=*/false);
    SyntheticSceneTap tap(focuser);
    FocusState state;
    state.SetAuto(0, true);
    state.SetAuto(1, true);
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    std::this_thread::sleep_for(kSettleWindow * 2);
    EXPECT_EQ(focuser.WriteCount(), 0U);  // no I2C ever
    EXPECT_EQ(tap.GrabCount(), 0);        // not even sampling — fully idle
    service.Stop();
}

TEST(AutofocusServiceTest, DispatcherFocusModeRoundTripFlipsTheLoop) {
    RecordingFocuser focuser;
    SyntheticSceneTap tap(focuser);
    tap.SetPeak(0, kCam0Peak);
    FocusState state;
    state.SetAuto(1, false);  // isolate camera 0
    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(std::make_shared<sst::control::CameraFocusHandler>(focuser, state));
    AutofocusService service(focuser, state, tap, nullptr, nullptr, FastConfig());
    service.Start();

    // Boot default is AUTO — the loop hunts immediately, no app command needed.
    ASSERT_TRUE(WaitForWriteCountAtLeast(focuser, 1));

    // MANUAL over the wire → the loop stands down; the manual write is applied.
    sst_cam::Command manual_cmd;
    auto* payload = manual_cmd.mutable_camera_focus();
    payload->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_MANUAL);
    payload->set_focus_position(kManualPosition);
    payload->set_camera_index(0);
    dispatcher.Dispatch(manual_cmd);
    const std::size_t settled = WaitForWriteQuiescence(focuser);

    std::this_thread::sleep_for(kSettleWindow * 2);
    EXPECT_EQ(focuser.WriteCount(), settled);  // no hunting after MANUAL
    EXPECT_FALSE(state.IsAuto(0));

    // AUTO over the wire → the loop resumes hunting camera 0.
    sst_cam::Command auto_cmd;
    auto_cmd.mutable_camera_focus()->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_AUTO);
    auto_cmd.mutable_camera_focus()->set_camera_index(0);
    const auto auto_resp = dispatcher.Dispatch(auto_cmd);
    EXPECT_EQ(auto_resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_TRUE(state.IsAuto(0));
    ASSERT_TRUE(WaitForWriteCountAtLeast(focuser, settled + 1));
    // (The deterministic "manual position is the last write" assertion lives in
    // ManualCommandMidSweepAbortsSweepAndManualHolds, which lock-steps the loop.)
    service.Stop();
}

}  // namespace
