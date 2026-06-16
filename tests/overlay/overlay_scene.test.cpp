// Overlay scene logic: bindings, template activation/expiry, {{param}}
// substitution, duration precedence, z-order (U8, R18/R19, AE3).
// Pure — no rendering.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "app/overlay/services/overlay_scene/overlay-scene.hpp"
#include "domain/overlay/models/overlay-enums.hpp"
#include "domain/overlay/models/overlay-layout.hpp"

namespace {

using namespace sst::overlay;

constexpr const char* kScoreVs = "2 – 1";  // en dash, matches ResolveText

constexpr float kElementWidth = 100;
constexpr float kElementHeight = 40;
constexpr std::uint32_t kFullHdWidth = 1920;
constexpr std::uint32_t kFullHdHeight = 1080;
constexpr std::uint32_t kGoalDurationMs = 3000;
constexpr float kBannerWidth = 400;
constexpr float kBannerHeight = 80;
constexpr std::uint32_t kBannerZ = 10;

auto TextElement(const std::string& element_id, OverlayBinding binding, std::uint32_t zorder)
    -> OverlayElement {
    OverlayElement element;
    element.id = element_id;
    element.shape = OverlayShape::kText;
    element.binding = binding;
    element.bounds =
        OverlayRect{.x1 = 0, .y1 = 0, .x2 = kElementWidth, .y2 = kElementHeight, .z = zorder};
    element.visible = true;
    return element;
}

auto FindText(const RenderScene& scene, const std::string& text) -> bool {
    return std::any_of(scene.elements.begin(), scene.elements.end(),
                       [&](const RenderElement& element) { return element.text == text; });
}

auto MakeLayout() -> OverlayLayout {
    OverlayLayout layout;
    layout.canvas_width = kFullHdWidth;
    layout.canvas_height = kFullHdHeight;
    layout.elements.push_back(TextElement("vs", OverlayBinding::kScoreVs, 2));
    layout.elements.push_back(TextElement("clock", OverlayBinding::kMatchClock, 1));
    layout.elements.push_back(TextElement("period", OverlayBinding::kPeriodLabel, 3));

    OverlayTemplate goal;
    goal.event_type = "goal";
    goal.duration_ms = kGoalDurationMs;
    OverlayElement banner;
    banner.id = "goal-text";
    banner.shape = OverlayShape::kText;
    banner.binding = OverlayBinding::kStatic;
    banner.bounds =
        OverlayRect{.x1 = 0, .y1 = 0, .x2 = kBannerWidth, .y2 = kBannerHeight, .z = kBannerZ};
    banner.style.static_text = "GOAL {{player}}";
    banner.visible = true;
    goal.elements.push_back(banner);
    layout.templates.push_back(goal);
    return layout;
}

// R18: bindings resolve from live data and re-render on update.
TEST(OverlaySceneTest, BindingsResolveFromLiveData) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());

    constexpr std::uint32_t kClockSeconds = 65;  // -> "01:05" in MM:SS
    BindingData data;
    data.score_a = 2;
    data.score_b = 1;
    data.clock_seconds = kClockSeconds;
    data.period = 1;
    data.period_state = PeriodLabelState::kInPlay;
    scene.SetBindingData(data);

    auto built = scene.Build(0);
    EXPECT_TRUE(FindText(built, kScoreVs));  // "2 – 1"
    EXPECT_TRUE(FindText(built, "01:05"));   // 65s -> MM:SS
    EXPECT_TRUE(FindText(built, "P1"));

    // Score change re-renders.
    data.score_a = 3;
    scene.SetBindingData(data);
    EXPECT_TRUE(FindText(scene.Build(0), "3 – 1"));
}

// R18: period-label state mapping.
TEST(OverlaySceneTest, PeriodLabelStateMapping) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());

    BindingData data;
    data.period = 2;
    data.period_state = PeriodLabelState::kInPlay;
    scene.SetBindingData(data);
    EXPECT_TRUE(FindText(scene.Build(0), "P2"));

    data.period_state = PeriodLabelState::kHalfTime;
    scene.SetBindingData(data);
    EXPECT_TRUE(FindText(scene.Build(0), "HT"));

    data.period_state = PeriodLabelState::kFullTime;
    scene.SetBindingData(data);
    EXPECT_TRUE(FindText(scene.Build(0), "FT"));
}

// AE3 / R19: banner substitutes {{param}}, is shown, then removed after its
// duration; command duration_s overrides template duration_ms.
TEST(OverlaySceneTest, BannerSubstitutionAndDurationOverride) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());

    ASSERT_TRUE(scene.ActivateBanner("goal", {{"player", "Smith"}}, /*duration_s_override=*/5,
                                     /*now_ms=*/0));
    EXPECT_TRUE(FindText(scene.Build(0), "GOAL Smith"));
    EXPECT_TRUE(FindText(scene.Build(4999), "GOAL Smith"));   // within 5 s
    EXPECT_FALSE(FindText(scene.Build(5000), "GOAL Smith"));  // expired (override wins)
}

// R19: with no duration override, the template's duration_ms is used.
TEST(OverlaySceneTest, BannerFallsBackToTemplateDuration) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());

    ASSERT_TRUE(scene.ActivateBanner("goal", {{"player", "Jones"}}, /*duration_s_override=*/0,
                                     /*now_ms=*/0));
    EXPECT_TRUE(FindText(scene.Build(2999), "GOAL Jones"));   // within 3000 ms
    EXPECT_FALSE(FindText(scene.Build(3000), "GOAL Jones"));  // expired
}

// Unknown template id is a no-op.
TEST(OverlaySceneTest, UnknownBannerIsNoop) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());
    EXPECT_FALSE(scene.ActivateBanner("nope", {}, 5, 0));
    // Build still works and shows only persistent elements.
    EXPECT_FALSE(FindText(scene.Build(0), "GOAL Smith"));
}

// Elements are flattened in ascending z order.
TEST(OverlaySceneTest, ElementsZOrdered) {
    OverlayScene scene;
    scene.SetLayout(MakeLayout());
    scene.SetBindingData(BindingData{});
    auto built = scene.Build(0);
    ASSERT_GE(built.elements.size(), 3U);
    for (std::size_t i = 1; i < built.elements.size(); ++i) {
        EXPECT_LE(built.elements[i - 1].bounds.z, built.elements[i].bounds.z);
    }
}

// Empty layout renders an empty scene.
TEST(OverlaySceneTest, EmptyLayoutIsEmptyScene) {
    constexpr std::uint32_t kHdWidth = 1280;
    constexpr std::uint32_t kHdHeight = 720;
    OverlayScene scene;
    OverlayLayout empty;
    empty.canvas_width = kHdWidth;
    empty.canvas_height = kHdHeight;
    scene.SetLayout(empty);
    auto built = scene.Build(0);
    EXPECT_TRUE(built.elements.empty());
    EXPECT_EQ(built.canvas_width, kHdWidth);
}

}  // namespace
