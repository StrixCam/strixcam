#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "../synthetic_frames.hpp"
#include "adapters/processing/opencv/opencv-preprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/color-mode.hpp"
#include "domain/processing/models/preprocess-config.hpp"

namespace sst::adapters::processing {

using sst::processing::ColorMode;
using sst::processing::PreprocessConfig;
using sst::tests::processing::MakeBgr8Frame;
using sst::tests::processing::MakeNv12Frame;

namespace {
// Neutral mid-scale chroma for synthetic NV12 frames.
constexpr std::uint8_t kNeutralChroma = 128;
// Threshold separating the binary-path "low" and "high" luminance cases.
constexpr std::uint8_t kMidThreshold = 127;

// Asserts every pixel in `frame`'s single plane equals `value`.
void ExpectPlaneAllEqual(const sst::capture::Frame& frame, std::uint8_t value) {
    for (std::size_t i = 0; i < frame.planes[0].size; ++i) {
        EXPECT_EQ(frame.planes[0].data[i], value);
    }
}

// Asserts every pixel in `frame`'s single plane is within `tol` of `value`.
void ExpectPlaneAllNear(const sst::capture::Frame& frame, std::uint8_t value, int tol) {
    for (std::size_t i = 0; i < frame.planes[0].size; ++i) {
        EXPECT_NEAR(frame.planes[0].data[i], value, tol);
    }
}
}  // namespace

TEST(OpenCvPreprocessorTest, RejectsNonNv12) {
    constexpr std::uint32_t kAiSize = 32;
    constexpr std::uint32_t kSrcSize = 64;
    OpenCvPreprocessor pre{PreprocessConfig{.ai_width = kAiSize, .ai_height = kAiSize}};
    auto out = pre.Process(MakeBgr8Frame(kSrcSize, kSrcSize, 0, 0, 0));
    EXPECT_FALSE(out.has_value());
}

TEST(OpenCvPreprocessorTest, GrayscalePathDimensionsAndFormat) {
    constexpr std::uint32_t kAiWidth = 32;
    constexpr std::uint32_t kAiHeight = 16;
    constexpr std::uint32_t kSrcWidth = 64;
    constexpr std::uint32_t kSrcHeight = 32;
    constexpr std::uint8_t kLuma = 128;
    constexpr int kTol = 2;
    OpenCvPreprocessor pre{PreprocessConfig{
        .ai_width = kAiWidth, .ai_height = kAiHeight, .ai_color_mode = ColorMode::Grayscale}};
    auto frame = MakeNv12Frame(kSrcWidth, kSrcHeight, kLuma, kNeutralChroma, kNeutralChroma);

    auto out = pre.Process(frame);
    ASSERT_TRUE(out.has_value());

    // source_frame is a value-copy of raw — still NV12.
    EXPECT_EQ(out->source_frame.format, sst::common::PixelFormat::NV12);
    EXPECT_EQ(out->source_frame.geometry.width, kSrcWidth);
    EXPECT_EQ(out->source_frame.geometry.height, kSrcHeight);
    EXPECT_EQ(out->source_frame.frame_id, frame.frame_id);

    // ai_frame is grayscale at the configured size.
    EXPECT_EQ(out->ai_frame.format, sst::common::PixelFormat::GRAY8);
    EXPECT_EQ(out->ai_frame.geometry.width, kAiWidth);
    EXPECT_EQ(out->ai_frame.geometry.height, kAiHeight);
    ASSERT_EQ(out->ai_frame.planes.size(), 1U);
    EXPECT_EQ(out->ai_frame.planes[0].stride, kAiWidth);
    EXPECT_EQ(out->ai_frame.frame_id, frame.frame_id);
    EXPECT_EQ(out->ai_frame.captured_at, frame.captured_at);

    // All Y pixels were kLuma, so ai pixels should also be ~kLuma.
    ExpectPlaneAllNear(out->ai_frame, kLuma, kTol);
}

TEST(OpenCvPreprocessorTest, BinaryPathThresholdsHigh) {
    constexpr std::uint32_t kAiSize = 16;
    constexpr std::uint32_t kSrcSize = 32;
    constexpr std::uint8_t kHighLuma = 200;
    constexpr std::uint8_t kWhite = 255;
    OpenCvPreprocessor pre{PreprocessConfig{.ai_width = kAiSize,
                                            .ai_height = kAiSize,
                                            .ai_color_mode = ColorMode::Binary,
                                            .binary_threshold = kMidThreshold}};
    auto out =
        pre.Process(MakeNv12Frame(kSrcSize, kSrcSize, kHighLuma, kNeutralChroma, kNeutralChroma));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->ai_frame.format, sst::common::PixelFormat::GRAY8);
    ExpectPlaneAllEqual(out->ai_frame, kWhite);
}

TEST(OpenCvPreprocessorTest, BinaryPathThresholdsLow) {
    constexpr std::uint32_t kAiSize = 16;
    constexpr std::uint32_t kSrcSize = 32;
    constexpr std::uint8_t kLowLuma = 50;
    OpenCvPreprocessor pre{PreprocessConfig{.ai_width = kAiSize,
                                            .ai_height = kAiSize,
                                            .ai_color_mode = ColorMode::Binary,
                                            .binary_threshold = kMidThreshold}};
    auto out =
        pre.Process(MakeNv12Frame(kSrcSize, kSrcSize, kLowLuma, kNeutralChroma, kNeutralChroma));
    ASSERT_TRUE(out.has_value());
    ExpectPlaneAllEqual(out->ai_frame, 0);
}

TEST(OpenCvPreprocessorTest, RgbPathChannelsAndOrder) {
    constexpr std::uint32_t kAiSize = 8;
    constexpr std::uint32_t kSrcSize = 32;
    // Red-ish in YUV (BT.601): Y=76, U=84, V=255.
    constexpr std::uint8_t kRedLuma = 76;
    constexpr std::uint8_t kRedU = 84;
    constexpr std::uint8_t kRedV = 255;
    constexpr int kRedHighFloor = 150;
    constexpr int kBlueLowCeil = 80;
    OpenCvPreprocessor pre{PreprocessConfig{
        .ai_width = kAiSize, .ai_height = kAiSize, .ai_color_mode = ColorMode::RGB}};
    auto out = pre.Process(MakeNv12Frame(kSrcSize, kSrcSize, kRedLuma, kRedU, kRedV));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->ai_frame.format, sst::common::PixelFormat::RGB8);
    ASSERT_EQ(out->ai_frame.planes.size(), 1U);
    EXPECT_EQ(out->ai_frame.planes[0].stride, kAiSize * 3U);

    const auto* pixels = out->ai_frame.planes[0].data;
    // R channel should be high, B channel should be low.
    EXPECT_GT(pixels[0], kRedHighFloor);  // R
    EXPECT_LT(pixels[2], kBlueLowCeil);   // B
}

TEST(OpenCvPreprocessorTest, StridedNv12RgbCopyFallback) {
    constexpr std::uint32_t kAiSize = 16;
    constexpr std::uint32_t kWidth = 32;
    constexpr std::uint32_t kHeight = 32;
    constexpr std::uint32_t kStridePad = 16;
    constexpr std::uint32_t kStride = kWidth + kStridePad;
    constexpr std::uint8_t kLuma = 100;
    OpenCvPreprocessor pre{PreprocessConfig{
        .ai_width = kAiSize, .ai_height = kAiSize, .ai_color_mode = ColorMode::RGB}};

    auto strided = MakeNv12Frame(kWidth, kHeight, kLuma, kNeutralChroma, kNeutralChroma, kStride);
    auto contig = MakeNv12Frame(kWidth, kHeight, kLuma, kNeutralChroma, kNeutralChroma);

    auto out_strided = pre.Process(strided);
    auto out_contig = pre.Process(contig);

    ASSERT_TRUE(out_strided.has_value());
    ASSERT_TRUE(out_contig.has_value());
    ASSERT_EQ(out_strided->ai_frame.planes[0].size, out_contig->ai_frame.planes[0].size);

    // Pixels from strided input must match the contiguous reference.
    EXPECT_EQ(0,
              std::memcmp(out_strided->ai_frame.planes[0].data, out_contig->ai_frame.planes[0].data,
                          out_strided->ai_frame.planes[0].size));
}

TEST(OpenCvPreprocessorTest, SourceFrameSharesOwnerWithRaw) {
    constexpr std::uint32_t kAiSize = 16;
    constexpr std::uint32_t kSrcSize = 32;
    OpenCvPreprocessor pre{PreprocessConfig{.ai_width = kAiSize, .ai_height = kAiSize}};
    auto raw = MakeNv12Frame(kSrcSize, kSrcSize, kNeutralChroma, kNeutralChroma, kNeutralChroma);

    const auto before = raw.owner.use_count();
    auto out = pre.Process(raw);
    ASSERT_TRUE(out.has_value());

    EXPECT_EQ(raw.owner.use_count(), before + 1);
    EXPECT_EQ(out->source_frame.owner, raw.owner);
    // Plane data pointers should be identical (no copy).
    EXPECT_EQ(out->source_frame.planes[0].data, raw.planes[0].data);
    EXPECT_EQ(out->source_frame.planes[1].data, raw.planes[1].data);
}

TEST(OpenCvPreprocessorTest, AiFrameOwnerOutlivesRaw) {
    constexpr std::uint32_t kAiSize = 16;
    constexpr std::uint32_t kSrcSize = 32;
    constexpr std::uint8_t kLuma = 200;
    constexpr int kTol = 2;
    OpenCvPreprocessor pre{PreprocessConfig{.ai_width = kAiSize, .ai_height = kAiSize}};
    auto out = [&] {
        auto raw = MakeNv12Frame(kSrcSize, kSrcSize, kLuma, kNeutralChroma, kNeutralChroma);
        auto bundle = pre.Process(raw);
        // Manually drop the source_frame's owner so the GstBuffer-equivalent
        // backing is gone. ai_frame must still be readable.
        bundle->source_frame.owner.reset();
        return bundle;
    }();

    ASSERT_TRUE(out.has_value());
    ASSERT_NE(out->ai_frame.owner, nullptr);
    ExpectPlaneAllNear(out->ai_frame, kLuma, kTol);
}

}  // namespace sst::adapters::processing
