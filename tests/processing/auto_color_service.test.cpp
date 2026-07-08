// Continuous auto-WB loop (AutoColorService): both cameras' gains converge
// toward the SHARED neutral target (matched color in any light), a manual
// calibration pauses the loop, enabled=false resumes it, and an unhealthy
// camera's gains hold. Frames come from a fake per-camera tap — no hardware.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <optional>
#include <thread>

#include "./synthetic_frames.hpp"
#include "app/pipeline/ports/camera-frame-tap.hpp"
#include "app/processing/services/auto-color/auto-color-service.hpp"
#include "domain/health/models/camera-health.hpp"
#include "domain/processing/models/auto-color-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/utils/auto-color.hpp"

namespace {

using sst::health::CameraHealth;
using sst::processing::AutoColorConfig;
using sst::processing::AutoColorMode;
using sst::processing::AutoColorService;
using sst::processing::AutoColorState;
using sst::processing::ChannelMeans;
using sst::processing::ColorCalibrationState;
using sst::processing::MeasureFrameMeans;
using sst::tests::processing::MakeNv12Frame;

// NOLINTBEGIN(readability-magic-numbers) — named test scenes / timings

constexpr std::uint32_t kSide = 64;
constexpr auto kDeadline = std::chrono::seconds(10);  // qemu headroom
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kSettleWindow = std::chrono::milliseconds(100);

// Two static per-camera casts: camera 0 greenish-dark, camera 1 reddish-bright
// (chroma AND luma mismatch — the shared target must fix both).
constexpr std::array<std::array<std::uint8_t, 3>, 2> kSceneYuv = {{
    {100, 120, 110},  // Y, U, V — camera 0
    {140, 130, 145},  // camera 1
}};

class FakeTap final : public sst::pipeline::ICameraFrameTap {
   public:
    auto GrabCameraFrame(std::size_t camera_index, std::chrono::milliseconds /*timeout*/)
        -> std::optional<sst::capture::Frame> override {
        if (camera_index >= kSceneYuv.size()) {
            return std::nullopt;
        }
        const auto& yuv = kSceneYuv.at(camera_index);
        return MakeNv12Frame(kSide, kSide, yuv[0], yuv[1], yuv[2]);
    }
};

auto FastConfig() -> AutoColorConfig {
    AutoColorConfig config;
    config.tick_interval = std::chrono::milliseconds(2);
    config.sample_timeout = std::chrono::milliseconds(50);
    return config;
}

auto SceneMeans(std::size_t camera) -> ChannelMeans {
    const auto& yuv = kSceneYuv.at(camera);
    const auto means = MeasureFrameMeans(MakeNv12Frame(kSide, kSide, yuv[0], yuv[1], yuv[2]));
    EXPECT_TRUE(means.has_value());
    return means.value_or(ChannelMeans{});
}

// The gains the loop must converge onto: every channel of every camera pulled
// to the shared target (the average of both cameras' luma).
struct ExpectedGains {
    float target;
    std::array<ChannelMeans, 2> means;
};
auto Expected() -> ExpectedGains {
    const auto means0 = SceneMeans(0);
    const auto means1 = SceneMeans(1);
    return {.target = (means0.Luma() + means1.Luma()) / 2.0F, .means = {means0, means1}};
}

auto GainsNear(const ColorCalibrationState::Gains& gains, const ChannelMeans& means, float target,
               float tolerance) -> bool {
    return std::fabs(gains.r - (target / means.r)) < tolerance &&
           std::fabs(gains.g - (target / means.g)) < tolerance &&
           std::fabs(gains.b - (target / means.b)) < tolerance;
}

auto WaitForConvergence(const ColorCalibrationState& calibration, const ExpectedGains& expected,
                        float tolerance) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        if (GainsNear(calibration.Get(0), expected.means[0], expected.target, tolerance) &&
            GainsNear(calibration.Get(1), expected.means[1], expected.target, tolerance)) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

class AutoColorServiceTest : public ::testing::Test {
   protected:
    void SetUp() override { ::unsetenv("SST_AUTO_COLOR_DISABLE"); }
    void TearDown() override { ::unsetenv("SST_AUTO_COLOR_DISABLE"); }
};

TEST_F(AutoColorServiceTest, BothCamerasConvergeToTheSharedTargetAndMatch) {
    FakeTap tap;
    ColorCalibrationState calibration(ColorCalibrationState::Gains{});
    AutoColorState mode;
    AutoColorService service(tap, calibration, mode, {}, FastConfig());
    service.Start();

    const auto expected = Expected();
    const float tolerance = FastConfig().tuning.dead_band + 0.005F;
    ASSERT_TRUE(WaitForConvergence(calibration, expected, tolerance));
    service.Stop();

    // Matched: the corrected means of BOTH cameras land on the same grey.
    for (std::size_t camera = 0; camera < 2; ++camera) {
        const auto gains = calibration.Get(camera);
        const auto& means = expected.means.at(camera);
        EXPECT_NEAR(gains.r * means.r, expected.target, expected.target * 0.05F);
        EXPECT_NEAR(gains.g * means.g, expected.target, expected.target * 0.05F);
        EXPECT_NEAR(gains.b * means.b, expected.target, expected.target * 0.05F);
        EXPECT_TRUE(gains.enabled);
    }
}

// Camera 0 serves exactly one frame ever, then every grab misses (the tap is
// shared with the AF sampler on metal and a contended grab loses the race).
class OneShotCam0Tap final : public sst::pipeline::ICameraFrameTap {
   public:
    auto GrabCameraFrame(std::size_t camera_index, std::chrono::milliseconds /*timeout*/)
        -> std::optional<sst::capture::Frame> override {
        if (camera_index >= kSceneYuv.size()) {
            return std::nullopt;
        }
        if (camera_index == 0 && cam0_served_.exchange(true)) {
            return std::nullopt;
        }
        const auto& yuv = kSceneYuv.at(camera_index);
        return MakeNv12Frame(kSide, kSide, yuv[0], yuv[1], yuv[2]);
    }

   private:
    std::atomic<bool> cam0_served_{false};
};

TEST_F(AutoColorServiceTest, CachedMeansKeepTheSharedTargetSteadyThroughMissedGrabs) {
    OneShotCam0Tap tap;
    ColorCalibrationState calibration(ColorCalibrationState::Gains{});
    AutoColorState mode;
    auto config = FastConfig();
    config.sample_attempts = 1;  // deterministic: a miss is a miss
    AutoColorService service(tap, calibration, mode, {}, config);
    service.Start();

    // Despite camera 0 sampling exactly once, its cached means keep it in the
    // shared target every tick — both cameras converge to the SAME two-camera
    // target as the always-fresh case (no one-camera target flapping).
    const auto expected = Expected();
    EXPECT_TRUE(WaitForConvergence(calibration, expected, config.tuning.dead_band + 0.005F));
    service.Stop();
}

TEST_F(AutoColorServiceTest, ManualModePausesAndReenableResumes) {
    FakeTap tap;
    // Seed a distinctive manual value the loop must NOT touch while manual.
    const ColorCalibrationState::Gains manual{
        .r = 0.5F, .g = 1.0F, .b = 1.5F, .enabled = true, .saturation = 1.3F};
    ColorCalibrationState calibration(manual);
    AutoColorState mode;
    mode.SetMode(AutoColorMode::kManual);
    AutoColorService service(tap, calibration, mode, {}, FastConfig());
    service.Start();

    std::this_thread::sleep_for(kSettleWindow);
    EXPECT_FLOAT_EQ(calibration.Get(0).r, 0.5F);  // untouched — user override holds
    EXPECT_FLOAT_EQ(calibration.Get(1).b, 1.5F);

    // Hand authority back: the loop takes over and converges.
    mode.SetMode(AutoColorMode::kAuto);
    const auto expected = Expected();
    EXPECT_TRUE(WaitForConvergence(calibration, expected, FastConfig().tuning.dead_band + 0.005F));
    // Tone survives the auto steps (auto-WB owns only the WB gains).
    EXPECT_FLOAT_EQ(calibration.Get(0).saturation, 1.3F);
    service.Stop();
}

TEST_F(AutoColorServiceTest, UnhealthyCameraHoldsWhileTheOtherTracks) {
    FakeTap tap;
    ColorCalibrationState calibration(ColorCalibrationState::Gains{});
    AutoColorState mode;
    // Camera 1 permanently DOWN: only camera 0 samples, so the shared target
    // degrades to camera 0's own luma (self-neutral chroma correction).
    AutoColorService service(
        tap, calibration, mode,
        [](std::size_t camera) { return camera == 0 ? CameraHealth::kOk : CameraHealth::kDown; },
        FastConfig());
    service.Start();

    const auto means0 = SceneMeans(0);
    const float target = means0.Luma();
    const float tolerance = FastConfig().tuning.dead_band + 0.005F;
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    bool converged = false;
    while (!converged && std::chrono::steady_clock::now() < deadline) {
        converged = GainsNear(calibration.Get(0), means0, target, tolerance);
        std::this_thread::sleep_for(kPollInterval);
    }
    EXPECT_TRUE(converged);
    service.Stop();

    // The skipped camera's gains never moved.
    EXPECT_FLOAT_EQ(calibration.Get(1).r, 1.0F);
    EXPECT_FLOAT_EQ(calibration.Get(1).g, 1.0F);
    EXPECT_FLOAT_EQ(calibration.Get(1).b, 1.0F);
}

TEST_F(AutoColorServiceTest, DisableEnvPinsStaticGains) {
    ::setenv("SST_AUTO_COLOR_DISABLE", "1", 1);
    FakeTap tap;
    ColorCalibrationState calibration(ColorCalibrationState::Gains{});
    AutoColorState mode;
    AutoColorService service(tap, calibration, mode, {}, FastConfig());
    service.Start();  // rollback hatch: no loop, no writes

    std::this_thread::sleep_for(kSettleWindow);
    service.Stop();
    EXPECT_FLOAT_EQ(calibration.Get(0).r, 1.0F);
    EXPECT_FLOAT_EQ(calibration.Get(1).b, 1.0F);
}

TEST_F(AutoColorServiceTest, StartStopIsIdempotentAndPrompt) {
    FakeTap tap;
    ColorCalibrationState calibration(ColorCalibrationState::Gains{});
    AutoColorState mode;
    AutoColorService service(tap, calibration, mode, {}, FastConfig());
    service.Start();
    service.Start();
    service.Stop();
    service.Stop();
    service.Start();
    service.Stop();
}

// NOLINTEND(readability-magic-numbers)

}  // namespace
