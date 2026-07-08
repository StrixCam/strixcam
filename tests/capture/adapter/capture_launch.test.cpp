// Capture launch builder (deterministic sensor color init): pure string-level
// assertions — no GStreamer elements, no hardware. The two IMX477s must boot
// to IDENTICAL predetermined AWB/AE values (per-sensor auto-algorithms settle
// independently and diverge — metal-measured ~48% channel-mean split with AE
// free), so the launch pins wbmode + exposure + analog gain + ISP digital
// gain, each env-tunable with compiled-in defaults.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "adapters/capture/frame/gstreamer/capture-launch.hpp"
#include "domain/capture/models/camera-config.hpp"

namespace {

using sst::capture::BuildCaptureLaunch;
using sst::capture::BuildSensorColorInitFragment;
using sst::capture::CameraConfig;

constexpr const char* kModel = "v1";
constexpr const char* kSink = "sink";

auto Contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

// Every color-init env cleared: tests below assert compiled-in defaults.
class CaptureLaunchTest : public ::testing::Test {
   protected:
    void SetUp() override { ClearEnv(); }
    void TearDown() override { ClearEnv(); }

    static void ClearEnv() {
        ::unsetenv("SST_CAPTURE_COLOR_INIT");
        ::unsetenv("SST_CAPTURE_WBMODE");
        ::unsetenv("SST_CAPTURE_EXPOSURE_NS");
        ::unsetenv("SST_CAPTURE_GAIN");
        ::unsetenv("SST_CAPTURE_ISP_DGAIN");
        ::unsetenv("SST_ISP_TUNING");
        ::unsetenv("SST_PIPELINE_FPS");
    }
};

TEST_F(CaptureLaunchTest, DefaultsPinWbExposureGainAndDigitalGain) {
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_TRUE(Contains(frag, "wbmode=5"));                               // daylight — fixed CCT
    EXPECT_TRUE(Contains(frag, "exposuretimerange=\"8333333 8333333\""));  // 1/120 s
    EXPECT_TRUE(Contains(frag, "gainrange=\"8 8\""));
    EXPECT_TRUE(Contains(frag, "ispdigitalgainrange=\"1 1\""));
}

TEST_F(CaptureLaunchTest, EnvKnobsOverrideEachPinnedValue) {
    ::setenv("SST_CAPTURE_WBMODE", "3", 1);
    ::setenv("SST_CAPTURE_EXPOSURE_NS", "4000000", 1);
    ::setenv("SST_CAPTURE_GAIN", "2.5", 1);
    ::setenv("SST_CAPTURE_ISP_DGAIN", "2", 1);
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_TRUE(Contains(frag, "wbmode=3"));
    EXPECT_TRUE(Contains(frag, "exposuretimerange=\"4000000 4000000\""));
    EXPECT_TRUE(Contains(frag, "gainrange=\"2.5 2.5\""));
    EXPECT_TRUE(Contains(frag, "ispdigitalgainrange=\"2 2\""));
}

TEST_F(CaptureLaunchTest, ZeroKnobLeavesThatDimensionOnAuto) {
    ::setenv("SST_CAPTURE_EXPOSURE_NS", "0", 1);
    ::setenv("SST_CAPTURE_GAIN", "0", 1);
    ::setenv("SST_CAPTURE_ISP_DGAIN", "0", 1);
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_TRUE(Contains(frag, "wbmode=5"));  // WB stays pinned
    EXPECT_FALSE(Contains(frag, "exposuretimerange"));
    EXPECT_FALSE(Contains(frag, "gainrange"));
    EXPECT_FALSE(Contains(frag, "ispdigitalgainrange"));
}

TEST_F(CaptureLaunchTest, WholeFragmentOverrideWinsVerbatim) {
    // The rollback hatch: full auto AWB/AE, ignoring the per-value knobs.
    ::setenv("SST_CAPTURE_COLOR_INIT", "wbmode=1", 1);
    ::setenv("SST_CAPTURE_GAIN", "4", 1);
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_EQ(frag, "wbmode=1");
}

TEST_F(CaptureLaunchTest, InvalidEnvValuesFallBackToDefaults) {
    ::setenv("SST_CAPTURE_WBMODE", "not-a-number", 1);
    ::setenv("SST_CAPTURE_EXPOSURE_NS", "soon", 1);
    ::setenv("SST_CAPTURE_GAIN", "loud", 1);
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_TRUE(Contains(frag, "wbmode=5"));
    EXPECT_TRUE(Contains(frag, "exposuretimerange=\"8333333 8333333\""));
    EXPECT_TRUE(Contains(frag, "gainrange=\"8 8\""));
}

TEST_F(CaptureLaunchTest, OutOfRangeWbModeIsClamped) {
    ::setenv("SST_CAPTURE_WBMODE", "42", 1);
    EXPECT_TRUE(Contains(BuildSensorColorInitFragment(), "wbmode=9"));
    ::setenv("SST_CAPTURE_WBMODE", "-3", 1);
    EXPECT_TRUE(Contains(BuildSensorColorInitFragment(), "wbmode=0"));
}

TEST_F(CaptureLaunchTest, LaunchSplicesColorInitBeforeIspTuningPerSensor) {
    const CameraConfig cfg{};
    for (std::uint16_t sensor = 0; sensor < 2; ++sensor) {
        const auto launch = BuildCaptureLaunch(cfg, kModel, sensor, kSink);
        ASSERT_FALSE(launch.empty());
        EXPECT_TRUE(Contains(launch, "nvarguscamerasrc sensor-id=" + std::to_string(sensor)));
        // Color init precedes the TNR/EE tuning, both on the source element.
        const auto color_at = launch.find("wbmode=5");
        const auto tnr_at = launch.find("tnr-mode=2");
        const auto caps_at = launch.find("! video/x-raw(memory:NVMM)");
        ASSERT_NE(color_at, std::string::npos);
        ASSERT_NE(tnr_at, std::string::npos);
        ASSERT_NE(caps_at, std::string::npos);
        EXPECT_LT(color_at, tnr_at);
        EXPECT_LT(tnr_at, caps_at);
        EXPECT_TRUE(Contains(launch, "exposuretimerange=\"8333333 8333333\""));
        EXPECT_TRUE(Contains(launch, "appsink name=sink sync=false"));
    }
}

TEST_F(CaptureLaunchTest, BothSensorsGetIdenticalColorInit) {
    // The whole point of the fix: sensor 0 and sensor 1 boot with the exact
    // same pinned values — only the sensor-id differs.
    const CameraConfig cfg{};
    auto launch0 = BuildCaptureLaunch(cfg, kModel, 0, kSink);
    auto launch1 = BuildCaptureLaunch(cfg, kModel, 1, kSink);
    ASSERT_FALSE(launch0.empty());
    ASSERT_FALSE(launch1.empty());
    const std::string id0 = "sensor-id=0";
    const std::string id1 = "sensor-id=1";
    launch1.replace(launch1.find(id1), id1.size(), id0);
    EXPECT_EQ(launch0, launch1);
}

TEST_F(CaptureLaunchTest, UnparseableModelYieldsEmptyLaunch) {
    const CameraConfig cfg{};
    EXPECT_TRUE(BuildCaptureLaunch(cfg, "garbage", 0, kSink).empty());
}

}  // namespace
