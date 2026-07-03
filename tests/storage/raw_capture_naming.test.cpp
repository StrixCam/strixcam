#include <gtest/gtest.h>

#include "domain/raw_capture/models/raw-capture-identity.hpp"
#include "domain/raw_capture/services/raw-capture-naming.hpp"

namespace {

using sst::raw_capture::RawCaptureIdentity;
namespace naming = sst::raw_capture::raw_capture_naming;

TEST(RawCaptureNamingTest, FileNameRoundTrips) {
    const RawCaptureIdentity identity{.capture_group_id = "abc-123-def", .camera_index = 1};
    const auto name = naming::FileName(identity);
    EXPECT_EQ(name, "raw__abc-123-def__cam1.mp4");

    const auto parsed = naming::ParseFileName(name);
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->capture_group_id, "abc-123-def");
    EXPECT_EQ(parsed->camera_index, 1U);
}

TEST(RawCaptureNamingTest, ParsesMultiDigitCameraIndex) {
    const auto parsed = naming::ParseFileName("raw__grp__cam12.mp4");
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->capture_group_id, "grp");
    EXPECT_EQ(parsed->camera_index, 12U);
}

TEST(RawCaptureNamingTest, RejectsNonRawNames) {
    EXPECT_FALSE(naming::ParseFileName("recording-2026.mp4").has_value());   // final: no raw__ prefix
    EXPECT_FALSE(naming::ParseFileName("raw__grp__cam0.nv12").has_value());  // wrong ext (proxy is .mp4)
    EXPECT_FALSE(naming::ParseFileName("grp__cam0.mp4").has_value());        // no prefix
    EXPECT_FALSE(naming::ParseFileName("raw__grp.mp4").has_value());         // no cam marker
    EXPECT_FALSE(naming::ParseFileName("raw__grp__camX.mp4").has_value());   // non-numeric index
    EXPECT_FALSE(naming::ParseFileName("raw____cam0.mp4").has_value());      // empty group
}

TEST(RawCaptureNamingTest, GroupIdMayContainSingleUnderscores) {
    const RawCaptureIdentity identity{.capture_group_id = "a_b_c", .camera_index = 0};
    const auto parsed = naming::ParseFileName(naming::FileName(identity));
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->capture_group_id, "a_b_c");
    EXPECT_EQ(parsed->camera_index, 0U);
}

}  // namespace
