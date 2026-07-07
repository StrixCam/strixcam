// VIC postprocess offload (state-health cycle U5): the internal
// appsrc ! nvvidconv ! appsink hop the postprocessor delegates NV12->BGR +
// scale to, and its software-fallback contract.
//
// Split in two suites:
// - VicConvertStageTest: environment-agnostic contract (input validation,
//   graceful nullopt when unavailable, postprocess parity between the
//   VIC-enabled and VIC-disabled construction). Passes in the container AND
//   on-device.
// - VicConvertStageHardwareE2E: requires the real Jetson VIC (nvvidconv).
//   EXPECTED TO FAIL in the cross-compile container (excluded by name in CI,
//   like every hardware-bound test); passes on-device.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "../synthetic_frames.hpp"
#include "adapters/processing/gstreamer/vic-convert-stage.hpp"
#include "adapters/processing/opencv/opencv-postprocessor.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace {

using sst::adapters::processing::OpenCvPostprocessor;
using sst::adapters::processing::VicConvertStage;
using sst::processing::CropRect;
using sst::processing::PostprocessConfig;
using sst::tests::processing::MakeBgr8Frame;
using sst::tests::processing::MakeNv12Frame;

// NOLINTBEGIN(readability-magic-numbers)
// Literals are self-evident synthetic geometry / pixel values.

// Invalid input is rejected up front — with or without the hardware present.
TEST(VicConvertStageTest, RejectsNonNv12AndBadGeometry) {
    VicConvertStage stage;
    EXPECT_FALSE(stage.Convert(MakeBgr8Frame(64, 64, 0, 0, 0), 32, 32).has_value());
    auto valid = MakeNv12Frame(64, 64, 128, 128, 128);
    EXPECT_FALSE(stage.Convert(valid, 0, 32).has_value());
    EXPECT_FALSE(stage.Convert(valid, 32, 0).has_value());
}

// When the stage reports unavailable (no nvvidconv — the dev container),
// Convert must return nullopt so the caller's software path takes over. On a
// Jetson this branch is simply not taken.
TEST(VicConvertStageTest, UnavailableStageConvertsNothing) {
    VicConvertStage stage;
    if (!stage.IsAvailable()) {
        auto frame = MakeNv12Frame(64, 64, 128, 128, 128);
        EXPECT_FALSE(stage.Convert(frame, 32, 32).has_value());
    }
}

// Calibration parity: a VIC-enabled postprocessor and an SST_DISABLE_VIC one
// must produce the same calibrated output for a neutral uniform frame. In the
// container both resolve to software (trivially equal, guarding the fallback
// path); on-device the VIC path is live and the neutral uniform frame is
// invariant to the scaler/matrix, so a small per-channel tolerance covers
// rounding differences. The calibration math itself (WB gains, saturation,
// contrast, brightness) runs identically after either convert.
TEST(VicConvertStageTest, PostprocessorOutputMatchesSoftwarePath) {
    const PostprocessConfig cfg{
        .output_width = 32, .output_height = 32, .output_format = sst::common::PixelFormat::BGR8};

    // VIC-enabled construction (env unset).
    unsetenv("SST_DISABLE_VIC");
    OpenCvPostprocessor with_vic{cfg};
    // Software-pinned construction.
    setenv("SST_DISABLE_VIC", "1", /*overwrite=*/1);
    OpenCvPostprocessor software{cfg};
    unsetenv("SST_DISABLE_VIC");

    // Neutral mid-grey NV12 (Y=128, U=V=128) — matrix- and scaler-invariant.
    auto frame = MakeNv12Frame(64, 64, 128, 128, 128);
    const CropRect full{0, 0, 64, 64};

    auto vic_out = with_vic.Process(frame, full);
    auto sw_out = software.Process(frame, full);
    if (!vic_out || !sw_out) {
        FAIL() << "Process returned nullopt";
        return;
    }
    const auto& vic_frame = *vic_out;
    const auto& sw_frame = *sw_out;
    ASSERT_EQ(vic_frame.geometry.width, sw_frame.geometry.width);
    ASSERT_EQ(vic_frame.geometry.height, sw_frame.geometry.height);
    ASSERT_EQ(vic_frame.planes[0].size, sw_frame.planes[0].size);

    constexpr int kTolerance = 3;  // rounding headroom between converters
    for (std::size_t i = 0; i < sw_frame.planes[0].size; ++i) {
        const int delta = static_cast<int>(vic_frame.planes[0].data[i]) -
                          static_cast<int>(sw_frame.planes[0].data[i]);
        ASSERT_LE(std::abs(delta), kTolerance) << "byte " << i;
    }
}

// ── Hardware-bound (real Jetson VIC) — expected to fail in the container ──

// The VIC hop converts + scales a real NV12 frame to BGR at the requested
// output geometry. Requires nvvidconv (Jetson).
TEST(VicConvertStageHardwareE2E, ConvertsAndScalesNv12OnVic) {
    VicConvertStage stage;
    ASSERT_TRUE(stage.IsAvailable());  // fails in the cross-compile container

    auto frame = MakeNv12Frame(128, 96, 128, 128, 128);
    auto bgr = stage.Convert(frame, 64, 48);
    if (!bgr) {
        FAIL() << "Convert returned nullopt";
        return;
    }
    const auto& mat = *bgr;
    EXPECT_EQ(mat.cols, 64);
    EXPECT_EQ(mat.rows, 48);
    EXPECT_EQ(mat.type(), CV_8UC3);

    // Neutral grey in, neutral grey out (any colour matrix).
    const auto center = mat.at<cv::Vec3b>(24, 32);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(center[channel], 128, 6);
    }

    // Geometry change rebuilds the internal pipeline and keeps converting.
    auto second = stage.Convert(frame, 32, 24);
    if (!second) {
        FAIL() << "Convert returned nullopt after geometry change";
        return;
    }
    const auto& second_mat = *second;
    EXPECT_EQ(second_mat.cols, 32);
    EXPECT_EQ(second_mat.rows, 24);
}

// NOLINTEND(readability-magic-numbers)

}  // namespace
