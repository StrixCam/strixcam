#include "domain/focus/services/sharpness.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace {

using sst::capture::Frame;
using sst::capture::FramePlane;
using sst::focus::SharpnessScore;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;
constexpr std::uint8_t kMidGrey = 128;

// Owned luma frame: a checkerboard of kMidGrey ± amplitude. Its Laplacian
// variance grows strictly with amplitude, which is what a lens moving toward
// focus does to real luma contrast.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) floor-ok: test helper — amplitude vs padding
auto MakeCheckerboardFrame(std::uint8_t amplitude, std::uint32_t stride_padding = 0) -> Frame {
    const std::uint32_t stride = kWidth + stride_padding;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(stride) * kHeight, kMidGrey);
    for (std::uint32_t py = 0; py < kHeight; ++py) {
        for (std::uint32_t px = 0; px < kWidth; ++px) {
            const bool white = ((px + py) % 2) == 0;
            (*pixels)[static_cast<std::size_t>(py) * stride + px] =
                white ? static_cast<std::uint8_t>(kMidGrey + amplitude)
                      : static_cast<std::uint8_t>(kMidGrey - amplitude);
        }
    }
    Frame frame;
    frame.format = sst::common::PixelFormat::GRAY8;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {.width = kWidth, .height = kHeight};
    frame.planes.push_back(
        FramePlane{.stride = stride, .data = pixels->data(), .size = pixels->size()});
    frame.owner = pixels;
    return frame;
}

constexpr std::uint8_t kSoftAmplitude = 10;
constexpr std::uint8_t kCrispAmplitude = 60;
constexpr std::uint8_t kBaseAmplitude = 40;

TEST(SharpnessScoreTest, SharperPatternScoresStrictlyHigher) {
    const auto soft = SharpnessScore(MakeCheckerboardFrame(kSoftAmplitude));
    const auto crisp = SharpnessScore(MakeCheckerboardFrame(kCrispAmplitude));
    ASSERT_TRUE(soft.has_value());
    ASSERT_TRUE(crisp.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_GT(*crisp, *soft);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_GT(*soft, 0.0);
}

TEST(SharpnessScoreTest, FlatFrameScoresZero) {
    const auto flat = SharpnessScore(MakeCheckerboardFrame(0));
    ASSERT_TRUE(flat.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_DOUBLE_EQ(*flat, 0.0);
}

TEST(SharpnessScoreTest, RespectsRowStridePadding) {
    // Same pattern, padded rows: the score must match the unpadded frame —
    // walking width instead of stride would smear rows together.
    constexpr std::uint32_t kPadding = 32;
    const auto unpadded = SharpnessScore(MakeCheckerboardFrame(kBaseAmplitude));
    const auto padded = SharpnessScore(MakeCheckerboardFrame(kBaseAmplitude, kPadding));
    ASSERT_TRUE(unpadded.has_value());
    ASSERT_TRUE(padded.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_DOUBLE_EQ(*unpadded, *padded);
}

TEST(SharpnessScoreTest, RejectsUnscoreableFrames) {
    EXPECT_FALSE(SharpnessScore(Frame{}).has_value());  // no planes at all

    auto non_cpu = MakeCheckerboardFrame(kBaseAmplitude);
    non_cpu.memory = sst::common::MemoryType::NVMM;  // can't dereference NVMM
    EXPECT_FALSE(SharpnessScore(non_cpu).has_value());

    auto packed_color = MakeCheckerboardFrame(kBaseAmplitude);
    packed_color.format = sst::common::PixelFormat::BGR8;  // plane 0 is not luma
    EXPECT_FALSE(SharpnessScore(packed_color).has_value());

    auto truncated = MakeCheckerboardFrame(kBaseAmplitude);
    truncated.planes[0].size = kWidth;  // plane too small for the geometry
    EXPECT_FALSE(SharpnessScore(truncated).has_value());
}

}  // namespace
