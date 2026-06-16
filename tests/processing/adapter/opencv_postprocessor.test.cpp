#include <gtest/gtest.h>

#include <cstdint>

#include "../synthetic_frames.hpp"
#include "adapters/processing/opencv/opencv-postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::adapters::processing {

using sst::processing::CropRect;
using sst::processing::PostprocessConfig;
using sst::tests::processing::MakeBgr8Frame;
using sst::tests::processing::MakeNv12Frame;

// Number of interleaved channels in a BGR8/RGB8 pixel. Used to compute byte
// offsets into the packed output plane.
constexpr std::size_t kBgrChannels = 3;

// Computes the byte offset of pixel (col, row) within a packed,
// `channels`-per-pixel image of the given row width. All arithmetic is done in
// std::size_t so the multiplication cannot overflow / implicitly widen from a
// narrower type when used as a pointer offset.
constexpr auto PixelOffset(std::size_t col, std::size_t row, std::size_t width,
                           std::size_t channels) -> std::size_t {
    return ((row * width) + col) * channels;
}

// NOLINTBEGIN(readability-magic-numbers)
// The literals below are self-evident synthetic test data: frame/crop
// dimensions, output resolutions, packed YUV/BGR byte values, and pixel sample
// indices. Naming each one would obscure rather than clarify the test intent.

TEST(OpenCvPostprocessorTest, RejectsNonNv12) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 32, .output_height = 32}};
    auto out = post.Process(MakeBgr8Frame(64, 64, 0, 0, 0), CropRect{0, 0, 32, 32});
    EXPECT_FALSE(out.has_value());
}

TEST(OpenCvPostprocessorTest, RejectsOutOfBoundsCrop) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 32, .output_height = 32}};
    auto src = MakeNv12Frame(100, 100, 128, 128, 128);
    EXPECT_FALSE(post.Process(src, CropRect{80, 80, 40, 40}).has_value());
    EXPECT_FALSE(post.Process(src, CropRect{0, 0, 0, 0}).has_value());
    EXPECT_FALSE(post.Process(src, CropRect{0, 0, 101, 100}).has_value());
}

TEST(OpenCvPostprocessorTest, CropAndResizeProducesExpectedDimsAndFormat) {
    OpenCvPostprocessor post{PostprocessConfig{
        .output_width = 50, .output_height = 50, .output_format = sst::common::PixelFormat::BGR8}};
    // Red-ish in YUV (BT.601): Y=76, U=84, V=255.
    auto src = MakeNv12Frame(200, 200, /*y=*/76, /*u=*/84, /*v=*/255);

    auto out = post.Process(src, CropRect{0, 0, 100, 100});
    ASSERT_TRUE(out.has_value());

    EXPECT_EQ(out->format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(out->geometry.width, 50U);
    EXPECT_EQ(out->geometry.height, 50U);
    ASSERT_EQ(out->planes.size(), 1U);
    EXPECT_EQ(out->planes[0].stride, 50U * 3U);
    EXPECT_EQ(out->planes[0].size, 50U * 50U * 3U);

    // Sample center pixel: B low, R high (BGR order).
    const auto* center = out->planes[0].data + PixelOffset(25, 25, 50, kBgrChannels);
    EXPECT_LT(center[0], 80);   // B
    EXPECT_GT(center[2], 150);  // R
}

TEST(OpenCvPostprocessorTest, RespectsSourceStride) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 16, .output_height = 16}};
    auto strided = MakeNv12Frame(64, 64, 100, 128, 128, /*stride=*/96);
    auto contig = MakeNv12Frame(64, 64, 100, 128, 128);

    auto out_strided = post.Process(strided, CropRect{0, 0, 32, 32});
    auto out_contig = post.Process(contig, CropRect{0, 0, 32, 32});

    ASSERT_TRUE(out_strided.has_value());
    ASSERT_TRUE(out_contig.has_value());
    EXPECT_EQ(out_strided->planes[0].size, out_contig->planes[0].size);

    // Sample center pixel of both — strided should match contig within
    // a small tolerance (chroma upsampling can vary at boundaries).
    const auto* strided_px = out_strided->planes[0].data + PixelOffset(8, 8, 16, kBgrChannels);
    const auto* contig_px = out_contig->planes[0].data + PixelOffset(8, 8, 16, kBgrChannels);
    EXPECT_NEAR(strided_px[0], contig_px[0], 2);
    EXPECT_NEAR(strided_px[1], contig_px[1], 2);
    EXPECT_NEAR(strided_px[2], contig_px[2], 2);
}

TEST(OpenCvPostprocessorTest, OutputFrameOwnsBuffer) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 16, .output_height = 16}};
    auto out = [&] {
        auto src = MakeNv12Frame(64, 64, 200, 128, 128);
        return post.Process(src, CropRect{0, 0, 32, 32});
    }();

    ASSERT_TRUE(out.has_value());
    ASSERT_NE(out->owner, nullptr);
    for (std::size_t i = 0; i < out->planes[0].size; ++i) {
        // After NV12→BGR with Y=200, U=V=128, all channels should be ~200.
        EXPECT_GT(out->planes[0].data[i], 150);
    }
}

TEST(OpenCvPostprocessorTest, OutputFormatGray8) {
    OpenCvPostprocessor post{PostprocessConfig{
        .output_width = 16, .output_height = 16, .output_format = sst::common::PixelFormat::GRAY8}};
    auto out = post.Process(MakeNv12Frame(64, 64, 200, 128, 128), CropRect{0, 0, 32, 32});

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->format, sst::common::PixelFormat::GRAY8);
    ASSERT_EQ(out->planes.size(), 1U);
    EXPECT_EQ(out->planes[0].stride, 16U);
    EXPECT_EQ(out->planes[0].size, 16U * 16U);
}

TEST(OpenCvPostprocessorTest, MetadataPropagated) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 16, .output_height = 16}};
    auto src = MakeNv12Frame(64, 64, 100, 128, 128, /*stride=*/0, /*frame_id=*/0xCAFE);

    auto out = post.Process(src, CropRect{0, 0, 32, 32});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->frame_id, src.frame_id);
    EXPECT_EQ(out->captured_at, src.captured_at);
}
// NOLINTEND(readability-magic-numbers)

}  // namespace sst::adapters::processing
