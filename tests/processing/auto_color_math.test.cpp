// Grey-world auto-color math (domain): channel means from NV12, the shared
// neutral target, and the damped/dead-banded step. Pure functions — the
// convergence/hysteresis guarantees of the continuous auto-WB loop live here.

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>

#include "./synthetic_frames.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/utils/auto-color.hpp"

namespace {

using sst::processing::AutoColorTuning;
using sst::processing::ChannelMeans;
using sst::processing::ColorCalibrationState;
using sst::processing::GreyWorldStep;
using sst::processing::MeasureFrameMeans;
using sst::processing::SharedNeutralTarget;
using sst::tests::processing::MakeBgr8Frame;
using sst::tests::processing::MakeNv12Frame;

// NOLINTBEGIN(readability-magic-numbers) — named test scenes / hand-computed expectations

TEST(AutoColorMathTest, NeutralGreyNv12FrameMeasuresEqualChannels) {
    // Y=128, U=V=128 is exact mid-grey in BT.601: R=G=B=128.
    const auto frame = MakeNv12Frame(64, 64, 128, 128, 128);
    const auto measured = MeasureFrameMeans(frame);
    ASSERT_TRUE(measured.has_value());
    const ChannelMeans means = measured.value_or(ChannelMeans{});
    EXPECT_NEAR(means.r, 128.0F, 0.5F);
    EXPECT_NEAR(means.g, 128.0F, 0.5F);
    EXPECT_NEAR(means.b, 128.0F, 0.5F);
    EXPECT_NEAR(means.Luma(), 128.0F, 0.5F);
}

TEST(AutoColorMathTest, ChromaCastMeasuresInTheRightDirection) {
    // V above bias pushes R up; U above bias pushes B up (BT.601).
    const ChannelMeans reddish =
        MeasureFrameMeans(MakeNv12Frame(64, 64, 128, 128, 160)).value_or(ChannelMeans{});
    EXPECT_GT(reddish.r, reddish.g);
    EXPECT_GT(reddish.r, reddish.b);

    const ChannelMeans blueish =
        MeasureFrameMeans(MakeNv12Frame(64, 64, 128, 160, 128)).value_or(ChannelMeans{});
    EXPECT_GT(blueish.b, blueish.r);
    EXPECT_GT(blueish.b, blueish.g);
}

// Builds an NV12 frame with two horizontal bands: a BRIGHT top band (luma
// `bright_y`, chroma bright_u/bright_v) over a DARK bottom band (luma `dark_y`,
// chroma dark_u/dark_v). Used to prove the white-patch estimator locks onto the
// LIT band's illuminant and ignores the dark shadow.
// NOLINTBEGIN(bugprone-easily-swappable-parameters) test builder, args by name
inline auto MakeTwoBandNv12(std::uint32_t width, std::uint32_t height, std::uint8_t bright_y,
                            std::uint8_t bright_u, std::uint8_t bright_v, std::uint8_t dark_y,
                            std::uint8_t dark_u, std::uint8_t dark_v) -> sst::capture::Frame {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::size_t y_size = static_cast<std::size_t>(width) * height;
    const std::size_t uv_size = static_cast<std::size_t>(width) * (height / 2);
    auto buf = std::make_shared<std::vector<std::uint8_t>>(y_size + uv_size);
    const std::uint32_t split = height / 2;  // top half bright, bottom half dark
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t band_y = (row < split) ? bright_y : dark_y;
        for (std::uint32_t col = 0; col < width; ++col) {
            (*buf)[static_cast<std::size_t>(row) * width + col] = band_y;
        }
    }
    for (std::uint32_t row = 0; row < height / 2; ++row) {
        const bool bright = (row < split / 2);
        const std::uint8_t chroma_u = bright ? bright_u : dark_u;
        const std::uint8_t chroma_v = bright ? bright_v : dark_v;
        for (std::uint32_t col = 0; col < width; col += 2) {
            (*buf)[y_size + static_cast<std::size_t>(row) * width + col + 0] = chroma_u;
            (*buf)[y_size + static_cast<std::size_t>(row) * width + col + 1] = chroma_v;
        }
    }
    sst::capture::Frame frame;
    frame.frame_id = 1;
    frame.format = sst::common::PixelFormat::NV12;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {width, height};
    frame.captured_at = sst::common::Timestamp{sst::tests::processing::kSyntheticCaptureTime};
    frame.planes.push_back(
        sst::capture::FramePlane{.stride = width, .data = buf->data(), .size = y_size});
    frame.planes.push_back(
        sst::capture::FramePlane{.stride = width, .data = buf->data() + y_size, .size = uv_size});
    frame.owner = std::shared_ptr<void>(buf);
    return frame;
}

// White-patch: the illuminant estimate comes from the BRIGHT (lit) band, not the
// whole-frame average. A green-lit top over a red-shadow bottom that averages
// near-neutral (the grey-world failure mode we hit on metal) must still report a
// GREEN cast so the loop actually corrects it.
TEST(AutoColorMathTest, HighlightWeightingLocksOntoLitBandNotDarkShadow) {
    // Bright top: green cast (U<128 and V<128 raise G, lower R and B in BT.601).
    // Dark bottom: red cast (V>128 raises R) — offsets the green in a full-frame
    // average, which is exactly what fooled grey-world.
    const auto frame = MakeTwoBandNv12(64, 64, /*bright_y=*/200, /*bright_u=*/100, /*bright_v=*/100,
                                       /*dark_y=*/36, /*dark_u=*/128, /*dark_v=*/168);
    const auto measured = MeasureFrameMeans(frame);
    ASSERT_TRUE(measured.has_value());
    const ChannelMeans means = measured.value_or(ChannelMeans{});
    // Estimator reflects the lit band -> green dominant (would be ~neutral if it
    // averaged the whole frame).
    EXPECT_GT(means.g, means.r) << "g=" << means.g << " r=" << means.r;
    EXPECT_GT(means.g, means.b) << "g=" << means.g << " b=" << means.b;
}

TEST(AutoColorMathTest, StridedNv12MeasuresSameAsContiguous) {
    const ChannelMeans strided =
        MeasureFrameMeans(MakeNv12Frame(48, 48, 100, 140, 120, /*stride=*/64))
            .value_or(ChannelMeans{});
    const ChannelMeans contig =
        MeasureFrameMeans(MakeNv12Frame(48, 48, 100, 140, 120)).value_or(ChannelMeans{});
    EXPECT_NEAR(strided.r, contig.r, 0.5F);
    EXPECT_NEAR(strided.g, contig.g, 0.5F);
    EXPECT_NEAR(strided.b, contig.b, 0.5F);
}

TEST(AutoColorMathTest, NonNv12FrameIsRejected) {
    EXPECT_FALSE(MeasureFrameMeans(MakeBgr8Frame(64, 64, 10, 20, 30)).has_value());
}

TEST(AutoColorMathTest, SharedTargetAveragesOnlyUsableCameras) {
    const std::array<std::optional<ChannelMeans>, 2> both = {
        ChannelMeans{.r = 90.0F, .g = 90.0F, .b = 90.0F},
        ChannelMeans{.r = 130.0F, .g = 130.0F, .b = 130.0F}};
    EXPECT_NEAR(SharedNeutralTarget(both).value_or(0.0F), 110.0F,
                0.01F);  // the midpoint — both cameras chase it

    const std::array<std::optional<ChannelMeans>, 2> one = {
        std::nullopt, ChannelMeans{.r = 130.0F, .g = 130.0F, .b = 130.0F}};
    EXPECT_NEAR(SharedNeutralTarget(one).value_or(0.0F), 130.0F, 0.01F);

    const std::array<std::optional<ChannelMeans>, 2> none = {std::nullopt, std::nullopt};
    EXPECT_FALSE(SharedNeutralTarget(none).has_value());
}

TEST(AutoColorMathTest, StepMovesAFractionTowardTheIdealGains) {
    const AutoColorTuning tuning{};                // alpha 0.25
    const ColorCalibrationState::Gains current{};  // all 1.0
    // Green-cast scene: G high → R/B gains should RISE toward target/mean.
    const ChannelMeans means{.r = 80.0F, .g = 120.0F, .b = 100.0F};
    const float target = 100.0F;

    const auto stepped = GreyWorldStep(current, means, target, tuning);
    ASSERT_TRUE(stepped.has_value());
    const ColorCalibrationState::Gains next = stepped.value_or(ColorCalibrationState::Gains{});
    // Ideal gains: r=1.25, g=0.8333, b=1.0. One damped step: 1 + 0.25*(ideal-1).
    EXPECT_NEAR(next.r, 1.0625F, 0.001F);
    EXPECT_NEAR(next.g, 0.9583F, 0.001F);
    EXPECT_NEAR(next.b, 1.0F, 0.001F);
    EXPECT_TRUE(next.enabled);
}

TEST(AutoColorMathTest, StepPreservesToneParameters) {
    const AutoColorTuning tuning{};
    ColorCalibrationState::Gains current{};
    current.saturation = 1.1F;
    current.contrast = 1.2F;
    current.brightness = -0.05F;
    const ChannelMeans means{.r = 80.0F, .g = 120.0F, .b = 100.0F};

    const auto stepped = GreyWorldStep(current, means, 100.0F, tuning);
    ASSERT_TRUE(stepped.has_value());
    const ColorCalibrationState::Gains next = stepped.value_or(ColorCalibrationState::Gains{});
    // Auto-WB owns only the WB gains — the baked tone stays applied on top.
    EXPECT_FLOAT_EQ(next.saturation, 1.1F);
    EXPECT_FLOAT_EQ(next.contrast, 1.2F);
    EXPECT_FLOAT_EQ(next.brightness, -0.05F);
}

TEST(AutoColorMathTest, RepeatedStepsConvergeWithinThirtyTicks) {
    // Simulate the loop against a STATIC cast: applied gains don't change the
    // sampled (pre-correction) means, so the ideal gain is fixed and the step
    // must converge onto it, then stop (dead-band).
    const AutoColorTuning tuning{};
    const ChannelMeans means{.r = 80.0F, .g = 120.0F, .b = 100.0F};
    const float target = 100.0F;

    ColorCalibrationState::Gains gains{};
    int applied_steps = 0;
    for (int tick = 0; tick < 30; ++tick) {
        const auto stepped = GreyWorldStep(gains, means, target, tuning);
        if (!stepped.has_value()) {
            break;  // converged — dead-band holds
        }
        gains = stepped.value_or(ColorCalibrationState::Gains{});
        ++applied_steps;
    }
    EXPECT_NEAR(gains.r, 1.25F, tuning.dead_band + 0.001F);
    EXPECT_NEAR(gains.g, 0.8333F, tuning.dead_band + 0.001F);
    EXPECT_NEAR(gains.b, 1.0F, tuning.dead_band + 0.001F);
    // And it stopped well before the tick budget (no perpetual writes).
    EXPECT_LT(applied_steps, 30);
    EXPECT_FALSE(GreyWorldStep(gains, means, target, tuning).has_value());
}

TEST(AutoColorMathTest, DeadBandSuppressesTinyCorrections) {
    const AutoColorTuning tuning{};  // dead_band 0.02
    // Current gains already within dead-band of ideal (1.01 vs ideal 1.0).
    ColorCalibrationState::Gains current{};
    current.r = 1.01F;
    current.g = 1.0F;
    current.b = 0.995F;
    const ChannelMeans means{.r = 100.0F, .g = 100.0F, .b = 100.0F};
    EXPECT_FALSE(GreyWorldStep(current, means, 100.0F, tuning).has_value());
}

TEST(AutoColorMathTest, IdealGainsClampToSliderRange) {
    const AutoColorTuning tuning{};  // clamp [0.3, 1.7]
    const ColorCalibrationState::Gains current{};
    // Extreme cast: ideal r = 200/10 = 20 → clamps to 1.7 before damping.
    const ChannelMeans means{.r = 10.0F, .g = 200.0F, .b = 200.0F};
    const auto stepped = GreyWorldStep(current, means, 200.0F, tuning);
    ASSERT_TRUE(stepped.has_value());
    const ColorCalibrationState::Gains next = stepped.value_or(ColorCalibrationState::Gains{});
    EXPECT_NEAR(next.r, 1.0F + (0.25F * (1.7F - 1.0F)), 0.001F);
    EXPECT_LE(next.r, tuning.max_gain);
}

TEST(AutoColorMathTest, DarkFrameIsUntrusted) {
    const AutoColorTuning tuning{};  // min_usable_mean 4.0
    const ColorCalibrationState::Gains current{};
    const ChannelMeans dark{.r = 2.0F, .g = 3.0F, .b = 1.0F};
    EXPECT_FALSE(GreyWorldStep(current, dark, 100.0F, tuning).has_value());
}

// NOLINTEND(readability-magic-numbers)

}  // namespace
