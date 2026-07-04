#include <gtest/gtest.h>

#include <cstdint>

#include "../synthetic_frames.hpp"
#include "adapters/processing/opencv/opencv-postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
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
    auto src = MakeNv12Frame(200, 200, /*luma=*/76, /*chroma_u=*/84, /*chroma_v=*/255);

    auto out = post.Process(src, CropRect{0, 0, 100, 100});
    if (!out) {
        FAIL() << "Process returned nullopt";
        return;
    }

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

    if (!out_strided) {
        FAIL() << "Process(strided) returned nullopt";
        return;
    }
    if (!out_contig) {
        FAIL() << "Process(contig) returned nullopt";
        return;
    }
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
    // Ownership/brightness test — disable the WB correction so the ~200 grey it
    // feeds stays ~200 on every channel (correction would cut B/R below 150).
    OpenCvPostprocessor post{PostprocessConfig{
        .output_width = 16, .output_height = 16, .color_correction = {.enabled = false}}};
    auto out = [&] {
        auto src = MakeNv12Frame(64, 64, 200, 128, 128);
        return post.Process(src, CropRect{0, 0, 32, 32});
    }();

    if (!out) {
        FAIL() << "Process returned nullopt";
        return;
    }
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

    if (!out) {
        FAIL() << "Process returned nullopt";
        return;
    }
    EXPECT_EQ(out->format, sst::common::PixelFormat::GRAY8);
    ASSERT_EQ(out->planes.size(), 1U);
    EXPECT_EQ(out->planes[0].stride, 16U);
    EXPECT_EQ(out->planes[0].size, 16U * 16U);
}

TEST(OpenCvPostprocessorTest, MetadataPropagated) {
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 16, .output_height = 16}};
    auto src = MakeNv12Frame(64, 64, 100, 128, 128, /*stride=*/0, /*frame_id=*/0xCAFE);

    auto out = post.Process(src, CropRect{0, 0, 32, 32});
    if (!out) {
        FAIL() << "Process returned nullopt";
        return;
    }
    EXPECT_EQ(out->frame_id, src.frame_id);
    EXPECT_EQ(out->captured_at, src.captured_at);
}
// U10: neutral grey NV12 (Y=U=V=128) demosaics to ~(128,128,128) BGR. A diagonal
// WB correction with explicit gains (B=0.5, G=1.0, R=0.5) must halve B and R while
// leaving G untouched — proving the per-channel gain is applied per channel and
// that disabling it leaves color alone.
TEST(OpenCvPostprocessorTest, ColorCorrectionAppliesPerChannelGain) {
    auto src = MakeNv12Frame(64, 64, 128, 128, 128);

    PostprocessConfig off{.output_width = 32, .output_height = 32};
    off.color_correction.enabled = false;
    PostprocessConfig enabled{.output_width = 32, .output_height = 32};
    enabled.color_correction = {.enabled = true, .r_gain = 0.5F, .g_gain = 1.0F, .b_gain = 0.5F};

    auto out_off = OpenCvPostprocessor{off}.Process(src, CropRect{0, 0, 64, 64});
    auto out_on = OpenCvPostprocessor{enabled}.Process(src, CropRect{0, 0, 64, 64});
    ASSERT_TRUE(out_off.has_value());
    ASSERT_TRUE(out_on.has_value());

    // floor-ok: ASSERT_TRUE(out_off.has_value()) guards this.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto* poff = out_off->planes[0].data + PixelOffset(16, 16, 32, kBgrChannels);
    // floor-ok: ASSERT_TRUE(out_on.has_value()) guards this.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto* pon = out_on->planes[0].data + PixelOffset(16, 16, 32, kBgrChannels);

    EXPECT_NEAR(pon[0], static_cast<double>(poff[0]) / 2, 4);  // B halved
    EXPECT_NEAR(pon[2], static_cast<double>(poff[2]) / 2, 4);  // R halved
    EXPECT_NEAR(pon[1], poff[1], 3);                           // G untouched (gain 1.0)
}

// The live calibration state (diagnostic sliders) overrides config gains and a
// mid-run Set() is reflected on the next frame — the mechanism the app relies on
// to retune the preview live.
TEST(OpenCvPostprocessorTest, LiveCalibrationStateOverridesConfigAndUpdates) {
    auto src = MakeNv12Frame(64, 64, 128, 128, 128);
    sst::processing::ColorCalibrationState calib(
        {.r = 0.5F, .g = 1.0F, .b = 0.5F, .enabled = true});
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 32, .output_height = 32}, &calib};

    auto out = post.Process(src, CropRect{0, 0, 64, 64});
    ASSERT_TRUE(out.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE(out.has_value())
    const auto* pix = out->planes[0].data + PixelOffset(16, 16, 32, kBgrChannels);
    EXPECT_LT(pix[0], 80);   // B halved from ~128
    EXPECT_LT(pix[2], 80);   // R halved
    EXPECT_GT(pix[1], 110);  // G ~unchanged

    calib.Set({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = true});  // sliders back to identity
    auto out2 = post.Process(src, CropRect{0, 0, 64, 64});
    ASSERT_TRUE(out2.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE(out2.has_value())
    const auto* pix2 = out2->planes[0].data + PixelOffset(16, 16, 32, kBgrChannels);
    EXPECT_GT(pix2[0], pix[0]);  // B restored on the next frame
}

// Saturation = 0 collapses a colored frame to greyscale (B≈G≈R); the WB gains
// stay at identity so only saturation acts.
TEST(OpenCvPostprocessorTest, SaturationZeroProducesGreyscale) {
    auto src = MakeNv12Frame(64, 64, 76, 84, 255);  // red-ish (BT.601: Y=76,U=84,V=255)
    sst::processing::ColorCalibrationState calib({.r = 1.0F,
                                                  .g = 1.0F,
                                                  .b = 1.0F,
                                                  .enabled = true,
                                                  .saturation = 0.0F,
                                                  .contrast = 1.0F,
                                                  .brightness = 0.0F});
    OpenCvPostprocessor post{PostprocessConfig{.output_width = 32, .output_height = 32}, &calib};

    auto out = post.Process(src, CropRect{0, 0, 64, 64});
    ASSERT_TRUE(out.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE(out.has_value())
    const auto* pix = out->planes[0].data + PixelOffset(16, 16, 32, kBgrChannels);
    EXPECT_NEAR(pix[0], pix[1], 6);  // B≈G
    EXPECT_NEAR(pix[1], pix[2], 6);  // G≈R → grey
}
// NOLINTEND(readability-magic-numbers)

}  // namespace sst::adapters::processing
