#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "app/decision/services/manual_decision/manual-decision.hpp"
#include "domain/decision/models/camera-choice.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/processing/models/frame-bundle.hpp"

namespace {

using sst::decision::ManualCameraState;
using sst::decision::ManualDecision;
using sst::processing::FrameBundle;

auto Bundle(std::uint32_t w, std::uint32_t h) -> FrameBundle {
    FrameBundle bundle;
    bundle.source_frame.geometry = {.width = w, .height = h};
    return bundle;
}

TEST(ManualDecisionTest, PicksTheSelectedCameraFullFrame) {
    ManualCameraState state;
    ManualDecision decision(state);
    std::vector<std::optional<FrameBundle>> cameras{Bundle(1920, 1080), Bundle(1280, 720)};

    state.Set(1);
    auto choice = decision.Decide(cameras);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->camera_index, 1U);
    EXPECT_EQ(choice->crop.width, 1280U);  // cam1's geometry, full frame

    state.Set(0);
    choice = decision.Decide(cameras);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->camera_index, 0U);
    EXPECT_EQ(choice->crop.width, 1920U);
}

TEST(ManualDecisionTest, FallsBackWhenSelectedCameraHasNoFrame) {
    ManualCameraState state;
    ManualDecision decision(state);
    // Selected camera (1) is absent this tick; camera 0 is present.
    std::vector<std::optional<FrameBundle>> cameras{Bundle(1920, 1080), std::nullopt};

    state.Set(1);
    const auto choice = decision.Decide(cameras);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->camera_index, 0U);  // fell back, output not blacked out
}

TEST(ManualDecisionTest, NulloptWhenNoCameraHasAFrame) {
    ManualCameraState state;
    ManualDecision decision(state);
    std::vector<std::optional<FrameBundle>> cameras{std::nullopt, std::nullopt};
    state.Set(0);
    EXPECT_FALSE(decision.Decide(cameras).has_value());
}

}  // namespace
