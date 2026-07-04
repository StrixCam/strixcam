// Training-proxy GStreamer launch-string builder (U4). Pure — asserts the
// pipeline shape/params without instantiating any GStreamer element.

#include <gtest/gtest.h>

#include <string>

#include "adapters/raw_capture/proxy-launch.hpp"
#include "domain/common/models/video-quality.hpp"

namespace {

using sst::adapters::raw_capture::BuildProxyLaunch;

auto Contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

// The proxy encodes the internal kProxyVideoMode (854x480@15), low-bitrate
// H.264 into MP4, with the leaky pre-encoder queue + I420 pin the software
// encoder needs, and block=false so PushCamera never stalls the fan-out thread.
TEST(ProxyLaunchTest, CarriesProxyModeEncodeChain) {
    const auto launch = BuildProxyLaunch("/videos/raw__g__cam0.mp4");
    EXPECT_TRUE(Contains(launch, "appsrc"));
    EXPECT_TRUE(Contains(launch, "block=false"));  // non-blocking push
    EXPECT_TRUE(Contains(launch, "width=854"));
    EXPECT_TRUE(Contains(launch, "height=480"));
    EXPECT_TRUE(Contains(launch, "framerate=15/1"));
    EXPECT_TRUE(Contains(launch, "format=I420"));  // 4:2:0 into x264enc
    EXPECT_TRUE(Contains(launch, "queue leaky=downstream"));
    EXPECT_TRUE(Contains(launch, "x264enc"));  // software encode (no NVENC)
    EXPECT_TRUE(Contains(launch, "bitrate=1500"));
    EXPECT_TRUE(Contains(launch, "key-int-max=30"));  // 15 fps * 2
    EXPECT_TRUE(Contains(launch, "mp4mux"));
    EXPECT_TRUE(Contains(launch, "filesink location=/videos/raw__g__cam0.mp4"));
    // Proxy dimensions must equal the single source of truth, not a magic literal.
    EXPECT_EQ(sst::common::kProxyVideoMode.width, 854);
    EXPECT_EQ(sst::common::kProxyVideoMode.height, 480);
    EXPECT_EQ(sst::common::kProxyVideoMode.fps, 15);
}

// VIC path: NV12 is VIC-native, so nvvidconv is used DIRECTLY (no BGRx repack,
// unlike the record/RTMP branches whose source is packed BGR). videoscale is
// gone (VIC scales); videorate stays software.
TEST(ProxyLaunchTest, VicPathUsesNvvidconvDirectlyNoBgrxRepack) {
    const auto vic = BuildProxyLaunch("/v/p.mp4", /*use_vic=*/true);
    EXPECT_TRUE(Contains(vic, "nvvidconv"));
    EXPECT_FALSE(Contains(vic, "videoscale"));  // scale on VIC
    EXPECT_FALSE(Contains(vic, "BGRx"));        // NV12 source needs no repack
    EXPECT_TRUE(Contains(vic, "videorate"));    // fps stays software
}

// Software fallback (SST_DISABLE_VIC): videoconvert + videoscale, no VIC.
TEST(ProxyLaunchTest, SoftwarePathUsesVideoconvertVideoscale) {
    const auto software = BuildProxyLaunch("/v/p.mp4", /*use_vic=*/false);
    EXPECT_TRUE(Contains(software, "videoconvert ! videoscale"));
    EXPECT_FALSE(Contains(software, "nvvidconv"));
    EXPECT_TRUE(Contains(software, "width=854"));
    EXPECT_TRUE(Contains(software, "format=I420"));
}

}  // namespace
