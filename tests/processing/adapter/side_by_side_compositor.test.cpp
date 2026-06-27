// Unit tests for the side-by-side preview compositor (#6 F6d): two BGR frames
// letterboxed into a fixed output canvas, left | right.
#include <gtest/gtest.h>

#include <cstdint>

#include "../synthetic_frames.hpp"
#include "adapters/processing/opencv/opencv-side-by-side-compositor.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace {

constexpr std::uint32_t kOutW = 1280;
constexpr std::uint32_t kOutH = 720;
constexpr std::size_t kBgrChannels = 3;
constexpr std::uint8_t kFull = 255;
constexpr std::uint8_t kNearFull = 200;  // INTER_AREA blends edges; center stays saturated

// Read one BGR pixel from a compact BGR8 frame.
struct Bgr {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
};
auto PixelAt(const sst::capture::Frame& frame, std::uint32_t col, std::uint32_t row) -> Bgr {
    const auto idx = (static_cast<std::size_t>(row) * frame.geometry.width + col) * kBgrChannels;
    const auto* d = frame.planes[0].data;
    return Bgr{d[idx + 0], d[idx + 1], d[idx + 2]};
}

}  // namespace

TEST(SideBySideCompositorTest, OutputMatchesCanvasGeometryAndFormat) {
    sst::adapters::processing::OpenCvSideBySideCompositor compositor(kOutW, kOutH);
    const auto left = sst::tests::processing::MakeBgr8Frame(1280, 720, kFull, 0, 0);   // blue
    const auto right = sst::tests::processing::MakeBgr8Frame(1280, 720, 0, 0, kFull);  // red

    const auto out = compositor.CompositeSideBySide(left, right);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->geometry.width, kOutW);
    EXPECT_EQ(out->geometry.height, kOutH);
    EXPECT_EQ(out->format, sst::common::PixelFormat::BGR8);
    ASSERT_FALSE(out->planes.empty());
    EXPECT_EQ(out->planes[0].size, static_cast<std::size_t>(kOutW) * kOutH * kBgrChannels);
}

TEST(SideBySideCompositorTest, LeftHalfIsLeftCameraRightHalfIsRightCamera) {
    sst::adapters::processing::OpenCvSideBySideCompositor compositor(kOutW, kOutH);
    const auto left = sst::tests::processing::MakeBgr8Frame(1280, 720, kFull, 0, 0);   // blue
    const auto right = sst::tests::processing::MakeBgr8Frame(1280, 720, 0, 0, kFull);  // red

    const auto out = compositor.CompositeSideBySide(left, right);
    ASSERT_TRUE(out.has_value());

    // A 1280x720 input letterboxes to 640x360 centered in its 640x720 column;
    // the column center (row 360) lands inside the image.
    const auto left_center = PixelAt(*out, kOutW / 4, kOutH / 2);
    EXPECT_GE(left_center.b, kNearFull);
    EXPECT_LE(left_center.r, kFull - kNearFull);

    const auto right_center = PixelAt(*out, (kOutW * 3) / 4, kOutH / 2);
    EXPECT_GE(right_center.r, kNearFull);
    EXPECT_LE(right_center.b, kFull - kNearFull);

    // Top edge of the canvas is letterbox black (image is vertically centered).
    const auto top = PixelAt(*out, kOutW / 4, 0);
    EXPECT_LE(top.b, kFull - kNearFull);
    EXPECT_LE(top.g, kFull - kNearFull);
    EXPECT_LE(top.r, kFull - kNearFull);
}

TEST(SideBySideCompositorTest, RejectsWhenBothInputsNonBgr) {
    sst::adapters::processing::OpenCvSideBySideCompositor compositor(kOutW, kOutH);
    // NV12 frames are not BGR8 — the compositor only consumes post-processed BGR.
    const auto nv12_a = sst::tests::processing::MakeNv12Frame(64, 64, 16, 128, 128);
    const auto nv12_b = sst::tests::processing::MakeNv12Frame(64, 64, 16, 128, 128);
    EXPECT_FALSE(compositor.CompositeSideBySide(nv12_a, nv12_b).has_value());
}

TEST(SideBySideCompositorTest, OneInvalidInputStillComposites) {
    sst::adapters::processing::OpenCvSideBySideCompositor compositor(kOutW, kOutH);
    const auto left = sst::tests::processing::MakeBgr8Frame(1280, 720, kFull, 0, 0);
    const auto nv12 = sst::tests::processing::MakeNv12Frame(64, 64, 16, 128, 128);  // invalid right
    const auto out = compositor.CompositeSideBySide(left, nv12);
    ASSERT_TRUE(out.has_value());
    // Right half is black (invalid input → letterbox black column).
    const auto right_center = PixelAt(*out, (kOutW * 3) / 4, kOutH / 2);
    EXPECT_LE(right_center.b, kFull - kNearFull);
    EXPECT_LE(right_center.r, kFull - kNearFull);
}
