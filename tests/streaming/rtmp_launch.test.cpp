// RTMP streamer GStreamer launch-string builder (U8). Pure — asserts the
// pipeline shape/params without instantiating any GStreamer element.

#include <gtest/gtest.h>

#include <string>

#include "adapters/streaming/gst_rtmp/rtmp-launch.hpp"
#include "domain/streaming/models/platform-stream-config.hpp"

namespace {

using sst::adapters::streaming::BuildRtmpLaunch;
using sst::adapters::streaming::BuildRtmpLocation;
using sst::streaming::PlatformStreamConfig;

auto Contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 30;
constexpr int kBitrateKbps = 4000;

auto MakeConfig() -> PlatformStreamConfig {
    PlatformStreamConfig cfg;
    cfg.url = "rtmp://ingest.example/live";
    cfg.stream_key = "secretkey";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.framerate = kFps;
    cfg.bitrate_kbps = kBitrateKbps;
    return cfg;
}

// The stream target resolution/fps ride on a per-branch videoscale/videorate,
// NOT on the appsrc input caps — the input caps are set lazily from the real
// frame at push time, so the launch string must carry no fixed input geometry.
TEST(RtmpLaunchTest, TargetDimensionsRideOnScalerNotInputCaps) {
    const auto launch = BuildRtmpLaunch(MakeConfig());
    EXPECT_TRUE(Contains(launch, "videoscale ! videorate"));
    EXPECT_TRUE(Contains(launch, "width=1280"));
    EXPECT_TRUE(Contains(launch, "height=720"));
    EXPECT_TRUE(Contains(launch, "framerate=30/1"));
    // No hardcoded appsrc input caps — those are derived from the frame at runtime.
    EXPECT_FALSE(Contains(launch, "caps="));
}

TEST(RtmpLaunchTest, CarriesEncodeAndMuxChain) {
    const auto launch = BuildRtmpLaunch(MakeConfig());
    EXPECT_TRUE(Contains(launch, "x264enc"));  // software encode (no NVENC)
    EXPECT_TRUE(Contains(launch, "bitrate=4000"));
    EXPECT_TRUE(Contains(launch, "key-int-max=60"));  // 30 fps * 2
    EXPECT_TRUE(Contains(launch, "flvmux"));
    EXPECT_TRUE(Contains(launch, "rtmp2sink"));
    EXPECT_TRUE(Contains(launch, "voaacenc"));  // silent AAC track (YouTube et al.)
    EXPECT_TRUE(Contains(launch, "location=\"rtmp://ingest.example/live/secretkey\""));
}

TEST(RtmpLaunchTest, LocationAppendsKeyAsFinalSegment) {
    auto cfg = MakeConfig();
    EXPECT_EQ(BuildRtmpLocation(cfg), "rtmp://ingest.example/live/secretkey");

    cfg.url = "rtmp://ingest.example/live/";  // trailing slash: no double slash
    EXPECT_EQ(BuildRtmpLocation(cfg), "rtmp://ingest.example/live/secretkey");

    cfg.url.clear();  // key-only fallback
    EXPECT_EQ(BuildRtmpLocation(cfg), "secretkey");
}

}  // namespace
