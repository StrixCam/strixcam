// Recorder GStreamer launch-string builder (U8). Pure — asserts the pipeline
// shape/params without instantiating any GStreamer element (hardware-free).

#include <gtest/gtest.h>

#include <string>

#include "adapters/storage/gstreamer/recorder-launch.hpp"
#include "domain/common/models/video-quality.hpp"

namespace {

using sst::adapters::storage::BuildRecorderLaunch;

auto Contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

// Unset quality → source resolution kept (no scaler), default framerate, I420
// forced for decodability, and the app-supplied path in the filesink.
TEST(RecorderLaunchTest, UnsetQualityKeepsSourceAndDefaults) {
    const auto launch = BuildRecorderLaunch("/videos/m/m.mp4", {});
    EXPECT_TRUE(Contains(launch, "video/x-raw,format=I420"));
    EXPECT_FALSE(Contains(launch, "videoscale"));
    EXPECT_FALSE(Contains(launch, "videorate"));
    EXPECT_TRUE(Contains(launch, "key-int-max=60"));  // default 30 fps * 2
    EXPECT_TRUE(Contains(launch, "filesink location=/videos/m/m.mp4"));
    EXPECT_TRUE(Contains(launch, "x264enc"));  // software encode (no NVENC)
}

// A pinned mode inserts the per-branch videoscale/videorate with the requested
// resolution/fps and a matching key-int interval.
TEST(RecorderLaunchTest, SetQualityInsertsScalerWithTargetDimensions) {
    const auto launch = BuildRecorderLaunch("/videos/m/m.mp4", {1920, 1080, 30});
    EXPECT_TRUE(Contains(launch, "videoscale ! videorate"));
    EXPECT_TRUE(Contains(launch, "width=1920"));
    EXPECT_TRUE(Contains(launch, "height=1080"));
    EXPECT_TRUE(Contains(launch, "framerate=30/1"));
    EXPECT_TRUE(Contains(launch, "format=I420"));
    EXPECT_TRUE(Contains(launch, "key-int-max=60"));
}

TEST(RecorderLaunchTest, KeyIntervalTracksRequestedFps) {
    const auto launch = BuildRecorderLaunch("/videos/m/m.mp4", {1280, 720, 60});
    EXPECT_TRUE(Contains(launch, "framerate=60/1"));
    EXPECT_TRUE(Contains(launch, "key-int-max=120"));  // 60 fps * 2
}

}  // namespace
