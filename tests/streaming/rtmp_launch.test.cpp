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
    // 4:2:0 pin (decodability) + a leaky pre-encoder queue so a slow software
    // encode drops frames instead of backing up the is-live appsrc unbounded.
    EXPECT_TRUE(Contains(launch, "format=I420"));
    EXPECT_TRUE(Contains(launch, "queue leaky=downstream"));
}

// Default is the proven software path; U2 use_vic=true offloads scale + convert
// to the VIC (nvvidconv), keeping videorate software and the mux/audio chain.
TEST(RtmpLaunchTest, VicOffloadUsesNvvidconv) {
    const auto software = BuildRtmpLaunch(MakeConfig());
    EXPECT_TRUE(Contains(software, "videoconvert ! videoscale"));
    EXPECT_FALSE(Contains(software, "nvvidconv"));

    const auto vic = BuildRtmpLaunch(MakeConfig(), /*use_vic=*/true);
    EXPECT_TRUE(Contains(vic, "nvvidconv"));
    EXPECT_TRUE(Contains(vic, "format=BGRx"));   // cheap repack for nvvidconv
    EXPECT_TRUE(Contains(vic, "videoconvert"));  // the repack only
    EXPECT_FALSE(Contains(vic, "videoscale"));   // scale moved to VIC
    EXPECT_TRUE(Contains(vic, "videorate"));     // fps stays software
    EXPECT_TRUE(Contains(vic, "width=1280"));
    EXPECT_TRUE(Contains(vic, "height=720"));
    EXPECT_TRUE(Contains(vic, "framerate=30/1"));
    EXPECT_TRUE(Contains(vic, "format=I420"));
    EXPECT_TRUE(Contains(vic, "x264enc"));
    EXPECT_TRUE(Contains(vic, "flvmux"));    // mux chain intact
    EXPECT_TRUE(Contains(vic, "voaacenc"));  // silent audio track intact
}

TEST(RtmpLaunchTest, LocationAppendsKeyAsFinalSegment) {
    auto cfg = MakeConfig();
    EXPECT_EQ(BuildRtmpLocation(cfg), "rtmp://ingest.example/live/secretkey");

    cfg.url = "rtmp://ingest.example/live/";  // trailing slash: no double slash
    EXPECT_EQ(BuildRtmpLocation(cfg), "rtmp://ingest.example/live/secretkey");

    cfg.url.clear();  // key-only fallback
    EXPECT_EQ(BuildRtmpLocation(cfg), "secretkey");
}

TEST(RtmpLaunchTest, LocationUsesUrlVerbatimWhenKeyInlined) {
    // The app inlines the key into the URL and sends no separate stream_key —
    // the location must be the URL as-is, never "<url>/" (a different stream).
    auto cfg = MakeConfig();
    cfg.url = "rtmp://ingest.example/live/mykey";
    cfg.stream_key.clear();
    EXPECT_EQ(BuildRtmpLocation(cfg), "rtmp://ingest.example/live/mykey");
}

// The stream runs the QUALITY profile (U3): tune=zerolatency is dropped and
// B-frames + lookahead are added — the same quality-per-bit levers as the
// recorder, paid for by the operator's accepted stream delay.
TEST(RtmpLaunchTest, UsesQualityProfileNotZerolatency) {
    const auto launch = BuildRtmpLaunch(MakeConfig());
    EXPECT_FALSE(Contains(launch, "tune=zerolatency")) << launch;
    EXPECT_TRUE(Contains(launch, "bframes=")) << launch;
    EXPECT_TRUE(Contains(launch, "rc-lookahead=")) << launch;
    // Dip-absorption buffer: time-deepened but still leaky (moov/backlog guard).
    EXPECT_FALSE(Contains(launch, "max-size-time=0 ")) << launch;
    EXPECT_TRUE(Contains(launch, "queue leaky=downstream")) << launch;
    // Silent-AAC branch preserved (video-only FLV is rejected downstream).
    EXPECT_TRUE(Contains(launch, "voaacenc")) << launch;
}

// The raised platform-stream default bitrate (kDefaultBitrateKbps 4000→14000)
// reaches the encode when the app supplies no explicit bitrate — the handler
// never sets bitrate_kbps from proto, so the constant is the real control point.
TEST(RtmpLaunchTest, RaisedDefaultBitrateReachesEncode) {
    PlatformStreamConfig cfg;  // all defaults, incl. bitrate_kbps=14000
    cfg.url = "rtmp://ingest.example/live";
    cfg.stream_key = "k";
    const auto launch = BuildRtmpLaunch(cfg);
    EXPECT_TRUE(Contains(launch, "bitrate=14000")) << launch;
}

// SST_STREAM_BITRATE_KBPS dials stream bitrate on metal without a rebuild
// (mirrors the recorder's SST_REC_BITRATE_KBPS); unset falls back to the config.
TEST(RtmpLaunchTest, StreamBitrateEnvOverride) {
    setenv("SST_STREAM_BITRATE_KBPS", "12000", /*overwrite=*/1);
    const auto overridden = BuildRtmpLaunch(MakeConfig());
    unsetenv("SST_STREAM_BITRATE_KBPS");
    EXPECT_TRUE(Contains(overridden, "bitrate=12000")) << overridden;

    const auto defaulted = BuildRtmpLaunch(MakeConfig());  // cfg's explicit 4000
    EXPECT_TRUE(Contains(defaulted, "bitrate=4000")) << defaulted;
}

// The pre-encode dip buffer depth is dialable on metal via SST_STREAM_QUEUE_MS.
TEST(RtmpLaunchTest, QueueDepthEnvOverride) {
    setenv("SST_STREAM_QUEUE_MS", "5000", /*overwrite=*/1);
    const auto launch = BuildRtmpLaunch(MakeConfig());
    unsetenv("SST_STREAM_QUEUE_MS");
    EXPECT_TRUE(Contains(launch, "max-size-time=5000000000")) << launch;
}

// 720p-stream fallback (R10) is config-only: the scale target follows
// cfg.width/height, so no code path is needed to drop the stream to 720p while
// the record master stays 1080p.
TEST(RtmpLaunchTest, SevenTwentyFallbackFollowsConfigDimensions) {
    auto cfg = MakeConfig();
    cfg.width = kWidth;   // 720p: stream scale target follows cfg dims (record master stays 1080p)
    cfg.height = kHeight;
    const auto launch = BuildRtmpLaunch(cfg);
    EXPECT_TRUE(Contains(launch, "width=1280")) << launch;
    EXPECT_TRUE(Contains(launch, "height=720")) << launch;
}

}  // namespace
