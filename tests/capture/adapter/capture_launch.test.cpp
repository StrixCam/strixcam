// Capture launch builder (sensor color init): pure string-level assertions —
// no GStreamer elements, no hardware. Default is FULL AUTO (wbmode auto, AE
// and gains free): the camera must render a usable image out of the box in
// ANY venue (pinned values only suit the venue they were dialed for — the
// pinned daylight/1/120s set measured ~28-count means + green cast indoors).
// Cross-sensor matching is the continuous software auto-WB loop's job. The
// SST_CAPTURE_* env knobs still pin any dimension for experiments.

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

TEST_F(CaptureLaunchTest, DefaultsAreFullAuto) {
    // Out-of-the-box any-venue: auto AWB, and NO pinned exposure/gain/dgain
    // ranges — AE adapts brightness to the venue.
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_EQ(frag, "wbmode=1");
    EXPECT_FALSE(Contains(frag, "exposuretimerange"));
    EXPECT_FALSE(Contains(frag, "gainrange"));
    EXPECT_FALSE(Contains(frag, "ispdigitalgainrange"));
}

TEST_F(CaptureLaunchTest, EnvKnobsPinEachDimension) {
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

TEST_F(CaptureLaunchTest, ExplicitZeroKnobStaysAuto) {
    ::setenv("SST_CAPTURE_EXPOSURE_NS", "0", 1);
    ::setenv("SST_CAPTURE_GAIN", "0", 1);
    ::setenv("SST_CAPTURE_ISP_DGAIN", "0", 1);
    EXPECT_EQ(BuildSensorColorInitFragment(), "wbmode=1");
}

TEST_F(CaptureLaunchTest, WholeFragmentOverrideWinsVerbatim) {
    // The experiment hatch: a fully pinned fragment, ignoring the per-value knobs.
    ::setenv("SST_CAPTURE_COLOR_INIT", "wbmode=5 gainrange=\"8 8\"", 1);
    ::setenv("SST_CAPTURE_GAIN", "4", 1);
    const auto frag = BuildSensorColorInitFragment();
    EXPECT_EQ(frag, "wbmode=5 gainrange=\"8 8\"");
}

TEST_F(CaptureLaunchTest, InvalidEnvValuesFallBackToAutoDefaults) {
    ::setenv("SST_CAPTURE_WBMODE", "not-a-number", 1);
    ::setenv("SST_CAPTURE_EXPOSURE_NS", "soon", 1);
    ::setenv("SST_CAPTURE_GAIN", "loud", 1);
    EXPECT_EQ(BuildSensorColorInitFragment(), "wbmode=1");
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
        const auto color_at = launch.find("wbmode=1");
        const auto tnr_at = launch.find("tnr-mode=2");
        const auto caps_at = launch.find("! video/x-raw(memory:NVMM)");
        ASSERT_NE(color_at, std::string::npos);
        ASSERT_NE(tnr_at, std::string::npos);
        ASSERT_NE(caps_at, std::string::npos);
        EXPECT_LT(color_at, tnr_at);
        EXPECT_LT(tnr_at, caps_at);
        // No pinned AE by default.
        EXPECT_FALSE(Contains(launch, "exposuretimerange"));
        EXPECT_TRUE(Contains(launch, "appsink name=sink sync=false"));
    }
}

TEST_F(CaptureLaunchTest, BothSensorsGetIdenticalColorInit) {
    // Sensor 0 and sensor 1 boot with the exact same fragment — only the
    // sensor-id differs (identical starting point for the auto algorithms).
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

// 4K supersampling: the sensor is read out at 3840x2160 (mode 0) and the VIC
// (nvvidconv) downscales to the delivered 1080p BEFORE the CPU-side pipeline —
// sharper + lower-noise than the 1080p60 binned mode, with no extra CPU cost
// because downstream only ever sees the delivered 1080p.
TEST_F(CaptureLaunchTest, ReadsSensorAt4kAndVicDownscalesToDelivered1080p) {
    const CameraConfig cfg{};  // defaults: sensor 3840x2160, delivered 1920x1080
    const auto launch = BuildCaptureLaunch(cfg, kModel, 0, kSink);
    ASSERT_FALSE(launch.empty());
    // Sensor caps (NVMM, off the argus source) carry the 4K readout geometry.
    EXPECT_TRUE(Contains(launch, "video/x-raw(memory:NVMM),width=3840,height=2160")) << launch;
    // nvvidconv (VIC) downscales to the delivered 1080p before the appsink.
    const auto nvvidconv_at = launch.find("nvvidconv");
    const auto delivered_at = launch.find("width=1920,height=1080");
    ASSERT_NE(nvvidconv_at, std::string::npos) << launch;
    ASSERT_NE(delivered_at, std::string::npos) << launch;
    EXPECT_LT(nvvidconv_at, delivered_at) << launch;  // downscale is on the VIC, post-source
}

TEST_F(CaptureLaunchTest, UnparseableModelYieldsEmptyLaunch) {
    const CameraConfig cfg{};
    EXPECT_TRUE(BuildCaptureLaunch(cfg, "garbage", 0, kSink).empty());
}

}  // namespace
