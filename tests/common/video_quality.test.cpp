// VideoQuality domain value + supported-mode validation (U8). Pure — no hardware.

#include <gtest/gtest.h>

#include "domain/common/models/video-quality.hpp"

namespace {

using sst::common::IsSupportedMode;
using sst::common::kSupportedVideoModes;
using sst::common::VideoQuality;

TEST(VideoQualityTest, IsSetRequiresAllPositive) {
    EXPECT_FALSE(VideoQuality{}.IsSet());
    EXPECT_FALSE((VideoQuality{1920, 1080, 0}).IsSet());
    EXPECT_FALSE((VideoQuality{0, 1080, 30}).IsSet());
    EXPECT_TRUE((VideoQuality{1280, 720, 30}).IsSet());
}

TEST(VideoQualityTest, EveryAdvertisedModeIsSupported) {
    for (const auto& mode : kSupportedVideoModes) {
        EXPECT_TRUE(IsSupportedMode(mode)) << mode.width << "x" << mode.height << "@" << mode.fps;
        EXPECT_TRUE(mode.IsSet());
    }
}

TEST(VideoQualityTest, UnadvertisedModesAreRejected) {
    EXPECT_FALSE(IsSupportedMode({3840, 2160, 30}));   // 4K
    EXPECT_FALSE(IsSupportedMode({1280, 720, 25}));    // right size, wrong fps
    EXPECT_FALSE(IsSupportedMode({1920, 1080, 120}));  // right size, wrong fps
    EXPECT_FALSE(IsSupportedMode({}));                 // unset is not "supported"
}

TEST(VideoQualityTest, EqualityIsComponentwise) {
    EXPECT_EQ((VideoQuality{1280, 720, 30}), (VideoQuality{1280, 720, 30}));
    EXPECT_NE((VideoQuality{1280, 720, 30}), (VideoQuality{1280, 720, 60}));
}

}  // namespace
