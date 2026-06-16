#include <gtest/gtest.h>

#include <cstdint>
#include <opencv2/core.hpp>

#include "../synthetic_frames.hpp"
#include "adapters/processing/opencv/frame-mat.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/common/models/timestamp.hpp"

namespace sst::adapters::processing {

using sst::tests::processing::MakeNv12Frame;

namespace {
// Mid-scale fill values used to synthesize neutral NV12 chroma.
constexpr std::uint8_t kNeutralChroma = 128;

// A NV12 region's luminance dimensions and its alternating chroma pair.
struct Nv12Pixels {
    cv::Size luma_size;  // width x height of the Y region
    std::uint8_t luma;
    std::uint8_t chroma_u;
    std::uint8_t chroma_v;
};

// Asserts every pixel in the top luma region equals `expected.luma`.
void ExpectYPlaneFilled(const cv::Mat& mat, const Nv12Pixels& expected) {
    for (int row = 0; row < expected.luma_size.height; ++row) {
        for (int col = 0; col < expected.luma_size.width; ++col) {
            EXPECT_EQ(mat.ptr(row)[col], expected.luma);
        }
    }
}

// Asserts UV rows (below the luma region) alternate (chroma_u, chroma_v).
void ExpectUvPlaneAlternates(const cv::Mat& mat, const Nv12Pixels& expected) {
    for (int row = expected.luma_size.height; row < mat.rows; ++row) {
        for (int col = 0; col < expected.luma_size.width; col += 2) {
            EXPECT_EQ(mat.ptr(row)[col + 0], expected.chroma_u);
            EXPECT_EQ(mat.ptr(row)[col + 1], expected.chroma_v);
        }
    }
}
}  // namespace

TEST(FrameMatTest, WrapNv12RejectsNonNv12) {
    constexpr std::uint32_t kSquare = 8;
    auto frame = sst::tests::processing::MakeBgr8Frame(kSquare, kSquare, 0, 0, 0);
    EXPECT_FALSE(WrapNv12(frame).has_value());
}

TEST(FrameMatTest, WrapNv12RejectsOddDimensions) {
    constexpr std::uint32_t kOddWidth = 7;
    constexpr std::uint32_t kEvenHeight = 8;
    // Odd width must be rejected; height/chroma are arbitrary valid fillers.
    auto frame = MakeNv12Frame(kOddWidth, kEvenHeight, kNeutralChroma, kNeutralChroma, kNeutralChroma);
    EXPECT_FALSE(WrapNv12(frame).has_value());
}

TEST(FrameMatTest, WrapNv12FastPathPointsAtInputMemory) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 32;
    constexpr std::uint8_t kLuma = 100;
    auto frame = MakeNv12Frame(kWidth, kHeight, kLuma, kNeutralChroma, kNeutralChroma);

    auto wrapped = WrapNv12(frame);
    ASSERT_TRUE(wrapped.has_value());
    EXPECT_EQ(wrapped->rows, 48);  // H * 3/2  NOLINT(readability-magic-numbers)
    EXPECT_EQ(wrapped->cols, static_cast<int>(kWidth));
    EXPECT_EQ(wrapped->type(), CV_8UC1);
    // Fast path: Mat row 0 must point at the input Y plane.
    EXPECT_EQ(wrapped->ptr(0), frame.planes[0].data);
}

TEST(FrameMatTest, WrapNv12CopiesWhenStrided) {
    constexpr std::uint32_t kWidth = 32;
    constexpr std::uint32_t kHeight = 16;
    constexpr std::uint32_t kStridePad = 16;
    constexpr std::uint32_t kStride = kWidth + kStridePad;
    constexpr std::uint8_t kLuma = 77;
    constexpr std::uint8_t kChromaU = 40;
    constexpr std::uint8_t kChromaV = 200;
    auto frame = MakeNv12Frame(kWidth, kHeight, kLuma, kChromaU, kChromaV, kStride);

    auto wrapped = WrapNv12(frame);
    ASSERT_TRUE(wrapped.has_value());
    EXPECT_EQ(wrapped->rows, 24);  // H * 3/2  NOLINT(readability-magic-numbers)
    EXPECT_EQ(wrapped->cols, static_cast<int>(kWidth));
    // Fallback path: Mat must NOT point at the strided input.
    EXPECT_NE(wrapped->ptr(0), frame.planes[0].data);

    const Nv12Pixels expected{
        .luma_size = cv::Size(static_cast<int>(kWidth), static_cast<int>(kHeight)),
        .luma = kLuma,
        .chroma_u = kChromaU,
        .chroma_v = kChromaV};
    ExpectYPlaneFilled(*wrapped, expected);
    ExpectUvPlaneAlternates(*wrapped, expected);
}

TEST(FrameMatTest, MakeOwnedFrameProducesCompactStride) {
    constexpr int kRows = 10;
    constexpr int kCols = 20;
    constexpr std::uint8_t kBlue = 11;
    constexpr std::uint8_t kGreen = 22;
    constexpr std::uint8_t kRed = 33;
    constexpr std::uint64_t kFrameId = 99;
    cv::Mat src(kRows, kCols, CV_8UC3, cv::Scalar(kBlue, kGreen, kRed));

    auto out = MakeOwnedFrame(src, sst::common::PixelFormat::BGR8, kFrameId, sst::common::Timestamp{});

    EXPECT_EQ(out.frame_id, kFrameId);
    EXPECT_EQ(out.format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(out.geometry.width, static_cast<std::uint32_t>(kCols));
    EXPECT_EQ(out.geometry.height, static_cast<std::uint32_t>(kRows));
    ASSERT_EQ(out.planes.size(), 1U);
    EXPECT_EQ(out.planes[0].stride, static_cast<std::uint32_t>(kCols) * 3U);
    EXPECT_EQ(out.planes[0].size, static_cast<std::size_t>(kRows) * kCols * 3U);
    ASSERT_NE(out.owner, nullptr);

    // Spot-check pixel values.
    EXPECT_EQ(out.planes[0].data[0], kBlue);
    EXPECT_EQ(out.planes[0].data[1], kGreen);
    EXPECT_EQ(out.planes[0].data[2], kRed);
}

}  // namespace sst::adapters::processing
