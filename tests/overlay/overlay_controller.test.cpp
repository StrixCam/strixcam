// Overlay push-on-change controller (U9, R18/R20). Pure — fake renderer + sink.

#include <gtest/gtest.h>

#include <cstdint>

#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/overlay/services/overlay_scene/overlay-scene.hpp"
#include "domain/overlay/models/overlay-enums.hpp"
#include "domain/overlay/models/overlay-layout.hpp"

namespace {

using sst::overlay::BindingData;
using sst::overlay::IOverlayRenderer;
using sst::overlay::IOverlaySink;
using sst::overlay::OverlayBinding;
using sst::overlay::OverlayController;
using sst::overlay::OverlayElement;
using sst::overlay::OverlayLayout;
using sst::overlay::OverlayRect;
using sst::overlay::OverlayShape;
using sst::overlay::RenderScene;
using sst::overlay::RgbaImage;

constexpr std::uint32_t kRgbaBytesPerPixel = 4;

class FakeRenderer final : public IOverlayRenderer {
   public:
    // Overrides the IOverlayRenderer port signature; the parameter order is fixed
    // by the interface and cannot be changed here.
    auto Render(
        const RenderScene& /*scene*/,
        std::uint32_t width,  // NOLINT(bugprone-easily-swappable-parameters) floor-ok: test double;
                              // order fixed by IOverlayRenderer::Render, cannot reorder in override
        std::uint32_t height) -> RgbaImage override {
        ++renders;
        RgbaImage img;
        img.width = width;
        img.height = height;
        img.stride = width * kRgbaBytesPerPixel;
        img.pixels.assign(static_cast<std::size_t>(img.stride) * height, 0);
        return img;
    }
    int renders{0};
};

class FakeSink final : public IOverlaySink {
   public:
    auto PushFrame(const RgbaImage& /*frame*/) -> void override { ++pushes; }
    auto Clear() -> void override { ++clears; }
    int pushes{0};
    int clears{0};
};

constexpr std::uint32_t kCanvasSize = 100;
constexpr float kElementWidth = 100;
constexpr float kElementHeight = 40;

auto ScoreLayout() -> OverlayLayout {
    OverlayLayout layout;
    layout.canvas_width = kCanvasSize;
    layout.canvas_height = kCanvasSize;
    OverlayElement vs_element;
    vs_element.id = "vs";
    vs_element.shape = OverlayShape::kText;
    vs_element.binding = OverlayBinding::kScoreVs;
    vs_element.bounds =
        OverlayRect{.x1 = 0, .y1 = 0, .x2 = kElementWidth, .y2 = kElementHeight, .z = 1};
    vs_element.visible = true;
    layout.elements.push_back(vs_element);
    return layout;
}

// A change pushes; an identical refresh does not (push-on-change).
TEST(OverlayControllerTest, PushesOnlyOnChange) {
    FakeRenderer renderer;
    FakeSink sink;
    OverlayController controller(renderer, sink, sst::common::OutputSize{kCanvasSize, kCanvasSize});
    controller.SetLayout(ScoreLayout());

    BindingData data;
    data.score_a = 1;
    data.score_b = 0;
    controller.SetBindingData(data);
    EXPECT_TRUE(controller.Refresh(0));  // first frame -> push
    EXPECT_EQ(sink.pushes, 1);

    // Same data, repeated refreshes: no new push (no thrash).
    controller.SetBindingData(data);
    EXPECT_FALSE(controller.Refresh(0));
    EXPECT_FALSE(controller.Refresh(0));
    EXPECT_EQ(sink.pushes, 1);

    // A real change pushes once more.
    data.score_a = 2;
    controller.SetBindingData(data);
    EXPECT_TRUE(controller.Refresh(0));
    EXPECT_EQ(sink.pushes, 2);
    EXPECT_EQ(renderer.renders, 2);  // rendered exactly per push
}

// Clear() drops the cached overlay and re-arms the push gate, so an identical
// refresh after a clear pushes again — the board reappears at kickoff after a
// match-end clear instead of being suppressed as "unchanged".
TEST(OverlayControllerTest, ClearDropsOverlayAndReArmsPush) {
    FakeRenderer renderer;
    FakeSink sink;
    OverlayController controller(renderer, sink, sst::common::OutputSize{kCanvasSize, kCanvasSize});
    controller.SetLayout(ScoreLayout());

    BindingData data;
    data.score_a = 1;
    controller.SetBindingData(data);
    EXPECT_TRUE(controller.Refresh(0));
    EXPECT_EQ(sink.pushes, 1);

    controller.Clear();
    EXPECT_EQ(sink.clears, 1);

    controller.SetBindingData(data);     // same data
    EXPECT_TRUE(controller.Refresh(0));  // still pushes (gate was re-armed)
    EXPECT_EQ(sink.pushes, 2);
}

// Between binding changes the compositor keeps the last buffer — the controller
// simply doesn't push, but the scene still reflects the overlay.
TEST(OverlayControllerTest, NoPushWhenNothingChanges) {
    FakeRenderer renderer;
    FakeSink sink;
    constexpr std::uint32_t kOutputSize = 64;
    constexpr int kRefreshAttempts = 5;
    OverlayController controller(renderer, sink, sst::common::OutputSize{kOutputSize, kOutputSize});
    controller.SetLayout(ScoreLayout());
    controller.SetBindingData(BindingData{});
    EXPECT_TRUE(controller.Refresh(0));
    const int after_first = sink.pushes;
    for (int i = 0; i < kRefreshAttempts; ++i) {
        EXPECT_FALSE(controller.Refresh(static_cast<std::uint64_t>(i)));
    }
    EXPECT_EQ(sink.pushes, after_first);
}

}  // namespace
