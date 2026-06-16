#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "domain/capture/models/frame.hpp"
#include "domain/overlay/models/render-scene.hpp"
#include "domain/overlay/services/frame-compositor.hpp"

namespace {

using sst::capture::Frame;
using sst::overlay::CompositeOverlay;
using sst::overlay::RgbaImage;

constexpr std::uint32_t kBgrBytesPerPixel = 3;
constexpr std::uint32_t kRgbaBytesPerPixel = 4;

// Strongly-typed image dimensions and pixel colors so the test helpers can't have
// their width/height or color channels transposed at a call site.
struct Size {
    std::uint32_t width;
    std::uint32_t height;
};
struct Bgr {
    std::uint8_t blue;
    std::uint8_t green;
    std::uint8_t red;
};
struct Rgba {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

// Keep the backing buffer alive alongside the Frame that points into it.
struct BgrFrame {
    Frame frame;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
};

auto MakeBgr(Size size, Bgr color) -> BgrFrame {
    const std::size_t pixel_count = static_cast<std::size_t>(size.width) * size.height;
    auto buf = std::make_shared<std::vector<std::uint8_t>>(pixel_count * kBgrBytesPerPixel);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        (*buf)[i * kBgrBytesPerPixel + 0] = color.blue;
        (*buf)[i * kBgrBytesPerPixel + 1] = color.green;
        (*buf)[i * kBgrBytesPerPixel + 2] = color.red;
    }
    Frame frame;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.geometry = {.width = size.width, .height = size.height};
    frame.planes = {sst::capture::FramePlane{
        .stride = size.width * kBgrBytesPerPixel, .data = buf->data(), .size = buf->size()}};
    frame.owner = buf;
    return {std::move(frame), buf};
}

auto MakeRgba(Size size, Rgba color) -> RgbaImage {
    const std::size_t pixel_count = static_cast<std::size_t>(size.width) * size.height;
    RgbaImage img;
    img.width = size.width;
    img.height = size.height;
    img.stride = size.width * kRgbaBytesPerPixel;
    img.pixels.resize(pixel_count * kRgbaBytesPerPixel);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        img.pixels[i * kRgbaBytesPerPixel + 0] = color.red;
        img.pixels[i * kRgbaBytesPerPixel + 1] = color.green;
        img.pixels[i * kRgbaBytesPerPixel + 2] = color.blue;
        img.pixels[i * kRgbaBytesPerPixel + 3] = color.alpha;
    }
    return img;
}

constexpr std::uint8_t kOpaque = 255;

// Named test colors, shared across the cases below.
constexpr Bgr kSrcBgr{.blue = 10, .green = 20, .red = 30};
constexpr Rgba kOverlayRgba{.red = 200, .green = 100, .blue = 50, .alpha = kOpaque};
// kOverlayRgba expressed in BGR byte order (what the composited plane should hold).
constexpr Bgr kOverlayBgr{.blue = 50, .green = 100, .red = 200};

// Assert every pixel of a tightly-packed BGR plane equals the expected color,
// within `tol` per channel. Kept out of the test bodies so each TEST stays below
// the cognitive-complexity threshold.
void ExpectAllPixelsBgr(const std::uint8_t* plane, std::size_t pixel_count, Bgr expected,
                        int tol = 0) {
    for (std::size_t i = 0; i < pixel_count; ++i) {
        EXPECT_NEAR(plane[i * kBgrBytesPerPixel + 0], expected.blue, tol);
        EXPECT_NEAR(plane[i * kBgrBytesPerPixel + 1], expected.green, tol);
        EXPECT_NEAR(plane[i * kBgrBytesPerPixel + 2], expected.red, tol);
    }
}

TEST(FrameCompositorTest, OpaqueOverlayReplacesPixels) {
    constexpr Size kSize{.width = 4, .height = 4};
    auto src = MakeBgr(kSize, kSrcBgr);
    const auto overlay = MakeRgba(kSize, kOverlayRgba);

    auto out = CompositeOverlay(src.frame, overlay);
    ASSERT_TRUE(out.has_value());
    ASSERT_FALSE(out->planes.empty());
    // Every pixel becomes the overlay color in BGR order (B=50, G=100, R=200).
    ExpectAllPixelsBgr(out->planes[0].data, static_cast<std::size_t>(kSize.width) * kSize.height,
                       kOverlayBgr);
}

TEST(FrameCompositorTest, FullyTransparentOverlayLeavesFrameUnchanged) {
    constexpr Size kSize{.width = 4, .height = 4};
    auto src = MakeBgr(kSize, kSrcBgr);
    // Same overlay color but fully transparent — the source must show through.
    const auto overlay = MakeRgba(
        kSize, Rgba{.red = kOverlayRgba.red, .green = kOverlayRgba.green, .blue = kOverlayRgba.blue,
                    .alpha = 0});

    auto out = CompositeOverlay(src.frame, overlay);
    ASSERT_TRUE(out.has_value());
    ExpectAllPixelsBgr(out->planes[0].data, static_cast<std::size_t>(kSize.width) * kSize.height,
                       kSrcBgr);
}

TEST(FrameCompositorTest, HalfAlphaBlendsHalfway) {
    constexpr Size kSize{.width = 2, .height = 2};
    constexpr std::uint8_t kHalfAlpha = 128;
    constexpr std::uint8_t kBlendedValue = 100;  // 0*(127/255) + 200*(128/255) ≈ 100
    constexpr int kBlendTolerance = 2;
    auto src = MakeBgr(kSize, Bgr{.blue = 0, .green = 0, .red = 0});
    const auto overlay =
        MakeRgba(kSize, Rgba{.red = 200, .green = 200, .blue = 200, .alpha = kHalfAlpha});

    auto out = CompositeOverlay(src.frame, overlay);
    ASSERT_TRUE(out.has_value());
    ExpectAllPixelsBgr(out->planes[0].data, static_cast<std::size_t>(kSize.width) * kSize.height,
                       Bgr{.blue = kBlendedValue, .green = kBlendedValue, .red = kBlendedValue},
                       kBlendTolerance);
}

TEST(FrameCompositorTest, NonBgrSourceReturnsNullopt) {
    constexpr Size kSize{.width = 4, .height = 4};
    auto src = MakeBgr(kSize, Bgr{.blue = 1, .green = 2, .red = 3});
    src.frame.format = sst::common::PixelFormat::NV12;  // unsupported
    EXPECT_FALSE(
        CompositeOverlay(src.frame, MakeRgba(kSize, Rgba{.red = 1, .green = 1, .blue = 1,
                                                         .alpha = kOpaque}))
            .has_value());
}

TEST(FrameCompositorTest, EmptyOverlayReturnsNullopt) {
    constexpr Size kSize{.width = 4, .height = 4};
    auto src = MakeBgr(kSize, Bgr{.blue = 1, .green = 2, .red = 3});
    RgbaImage empty;  // 0x0, no pixels
    EXPECT_FALSE(CompositeOverlay(src.frame, empty).has_value());
}

TEST(FrameCompositorTest, MismatchedAspectIsLetterboxedAndCentered) {
    // Wide 4x2 frame (2:1), square 2x2 overlay (1:1): aspect-preserving scale =
    // min(4/2, 2/2) = 1, so the 2x2 overlay is drawn centered at x in [1,3) with
    // pillarbox margins at x=0 and x=3.
    constexpr Size kFrameSize{.width = 4, .height = 2};
    constexpr Size kOverlaySize{.width = 2, .height = 2};
    auto src = MakeBgr(kFrameSize, Bgr{.blue = 0, .green = 0, .red = 0});
    const auto overlay =
        MakeRgba(kOverlaySize, Rgba{.red = 255, .green = 255, .blue = 255, .alpha = kOpaque});

    auto out = CompositeOverlay(src.frame, overlay);
    ASSERT_TRUE(out.has_value());
    const auto* plane = out->planes[0].data;
    const auto blue_at = [&](std::uint32_t col, std::uint32_t row) {
        return plane[(static_cast<std::size_t>(row) * kFrameSize.width + col) * kBgrBytesPerPixel];
    };
    EXPECT_EQ(blue_at(0, 0), 0);          // left margin — untouched black
    EXPECT_EQ(blue_at(3, 0), 0);          // right margin — untouched black
    EXPECT_EQ(blue_at(1, 0), kOpaque);    // drawn region — white
    EXPECT_EQ(blue_at(2, 1), kOpaque);    // drawn region — white
}

}  // namespace
