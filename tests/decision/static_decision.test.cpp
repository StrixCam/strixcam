#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "app/decision/services/static_decision/static-decision.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/decision/models/camera-choice.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace {

using sst::capture::Frame;
using sst::decision::CameraChoice;
using sst::decision::StaticDecision;
using sst::processing::FrameBundle;

// A frame geometry paired so width/height can't be swapped at the call site.
struct Size {
    std::uint32_t width;
    std::uint32_t height;
};

// Build a bundle whose source frame carries the given geometry. StaticDecision
// only reads source geometry, so the planes/owner are intentionally empty.
auto BundleWithGeometry(Size size) -> FrameBundle {
    FrameBundle bundle;
    bundle.source_frame.geometry = {.width = size.width, .height = size.height};
    return bundle;
}

// The off-camera (cam 1) geometry used wherever a present-but-ignored second
// camera is needed; distinct from any cam-0 size so a wrong pick is visible.
// NOLINTNEXTLINE(readability-magic-numbers) — self-evident 800x600 placeholder dims
constexpr Size kOtherCameraSize{800, 600};

// Assert that `choice` is a full-frame selection of camera 0 covering `size`.
// Extracted from the table-driven test to keep its loop body — and the
// expanded gtest macros — under the cognitive-complexity threshold.
void ExpectFullFrameCamera0(const std::optional<CameraChoice>& choice, Size size) {
    ASSERT_TRUE(choice.has_value())
        << "expected a choice for " << size.width << "x" << size.height;
    EXPECT_EQ(choice->camera_index, 0U);
    EXPECT_EQ(choice->crop.x, 0U);
    EXPECT_EQ(choice->crop.y, 0U);
    EXPECT_EQ(choice->crop.width, size.width);
    EXPECT_EQ(choice->crop.height, size.height);
}

TEST(StaticDecisionTest, PicksCamera0FullFrameForVariousSizes) {
    StaticDecision decision;
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident resolutions + edge dims
    const std::vector<Size> sizes = {{1920, 1080}, {1280, 720}, {640, 480}, {3840, 2160}, {1, 1}};

    for (const auto& size : sizes) {
        std::vector<std::optional<FrameBundle>> cameras = {
            BundleWithGeometry(size), BundleWithGeometry(kOtherCameraSize)};
        ExpectFullFrameCamera0(decision.Decide(cameras), size);
    }
}

TEST(StaticDecisionTest, IgnoresCamera1WhenCamera0Present) {
    StaticDecision decision;
    std::vector<std::optional<FrameBundle>> cameras = {
        // NOLINTNEXTLINE(readability-magic-numbers) — self-evident 1080p/720p dims
        BundleWithGeometry({1920, 1080}), BundleWithGeometry({1280, 720})};
    const auto choice = decision.Decide(cameras);
    ASSERT_TRUE(choice.has_value());
    // Always cam 0 even though cam 1 also has a (differently-sized) frame.
    EXPECT_EQ(choice->camera_index, 0U);
    EXPECT_EQ(choice->crop.width, 1920U);   // NOLINT(readability-magic-numbers) — 1080p width
    EXPECT_EQ(choice->crop.height, 1080U);  // NOLINT(readability-magic-numbers) — 1080p height
}

TEST(StaticDecisionTest, ReturnsNulloptWhenCamera0Absent) {
    StaticDecision decision;
    // Camera 0 has no frame this tick; camera 1 does. Static policy serves only
    // camera 0, so there is nothing to present.
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident 720p dims
    std::vector<std::optional<FrameBundle>> cameras = {std::nullopt, BundleWithGeometry({1280, 720})};
    EXPECT_FALSE(decision.Decide(cameras).has_value());
}

TEST(StaticDecisionTest, ReturnsNulloptWhenNoCameras) {
    StaticDecision decision;
    const std::vector<std::optional<FrameBundle>> empty;
    EXPECT_FALSE(decision.Decide(empty).has_value());
}

TEST(StaticDecisionTest, DegenerateGeometryIsDefinedNotCrash) {
    StaticDecision decision;
    // Zero width or height → defined behavior: skip the tick, no crash.
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident degenerate/1080p dims
    std::vector<std::optional<FrameBundle>> zero_width = {BundleWithGeometry({0, 1080})};
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident degenerate/1080p dims
    std::vector<std::optional<FrameBundle>> zero_height = {BundleWithGeometry({1920, 0})};
    std::vector<std::optional<FrameBundle>> zero_both = {BundleWithGeometry({0, 0})};
    EXPECT_FALSE(decision.Decide(zero_width).has_value());
    EXPECT_FALSE(decision.Decide(zero_height).has_value());
    EXPECT_FALSE(decision.Decide(zero_both).has_value());
}

}  // namespace
