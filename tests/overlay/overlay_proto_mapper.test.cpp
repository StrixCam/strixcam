// Proto -> domain overlay mapping (U1, R1).
//
// Pure — no rendering, no transport. Verifies the proto3-`optional`
// default-inversion fix: an OverlayElement.visible / OverlayStyle.opacity that
// the app leaves UNSET must map to the documented defaults (visible=true,
// opacity=1.0), not the scalar zero-values (false / 0.0) that would otherwise
// drop or transparentize the element. See proto/overlay-rendering.md
// "Element defaults".

#include <gtest/gtest.h>

#include <cstdint>

#include "app/overlay/overlay-proto-mapper.hpp"
#include "bluetooth.pb.h"
#include "domain/overlay/models/overlay-layout.hpp"

namespace {

using sst::overlay::MapLayoutFromProto;

constexpr std::int32_t kCanvasSize = 100;
constexpr float kHalfOpacity = 0.5F;
constexpr float kAboveMaxOpacity = 1.5F;   // > 1.0, expected to clamp to 1.0
constexpr float kBelowMinOpacity = -0.2F;  // < 0.0, expected to clamp to 0.0

// Build a one-element proto layout and return the mapped domain element.
auto MapSingle(const sst_cam::OverlayElement& proto_el) -> sst::overlay::OverlayElement {
    sst_cam::OverlayLayout layout;
    layout.set_canvas_width(kCanvasSize);
    layout.set_canvas_height(kCanvasSize);
    *layout.add_elements() = proto_el;
    auto domain = MapLayoutFromProto(layout);
    EXPECT_EQ(domain.elements.size(), 1U);
    return domain.elements.front();
}

// Happy path: visible explicitly set to false maps through as false.
TEST(OverlayProtoMapperTest, ExplicitVisibleFalseIsPreserved) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    element.set_visible(false);
    EXPECT_FALSE(MapSingle(element).visible);
}

// Blocker fix: an element with visible UNSET maps to visible=true (renders),
// not the proto3 scalar zero (false) that previously dropped it.
TEST(OverlayProtoMapperTest, UnsetVisibleDefaultsToTrue) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    ASSERT_FALSE(element.has_visible());
    EXPECT_TRUE(MapSingle(element).visible);
}

// Explicit visible=true still maps to true.
TEST(OverlayProtoMapperTest, ExplicitVisibleTrueIsPreserved) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    element.set_visible(true);
    EXPECT_TRUE(MapSingle(element).visible);
}

// Blocker fix: a style with opacity UNSET maps to 1.0 (opaque), not 0.0
// (fully transparent).
TEST(OverlayProtoMapperTest, UnsetOpacityDefaultsToOpaque) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    ASSERT_FALSE(element.style().has_opacity());
    EXPECT_FLOAT_EQ(MapSingle(element).style.opacity, 1.0F);
}

// An explicit opacity is preserved verbatim (including a legitimate 0.0).
TEST(OverlayProtoMapperTest, ExplicitOpacityIsPreserved) {
    sst_cam::OverlayElement half;
    half.set_id("e");
    half.mutable_style()->set_opacity(kHalfOpacity);
    EXPECT_FLOAT_EQ(MapSingle(half).style.opacity, kHalfOpacity);

    sst_cam::OverlayElement zero;
    zero.set_id("e");
    zero.mutable_style()->set_opacity(0.0F);
    ASSERT_TRUE(zero.style().has_opacity());
    EXPECT_FLOAT_EQ(MapSingle(zero).style.opacity, 0.0F);
}

// #6: an opacity outside [0,1] is clamped at the mapper (the single proto->domain
// translation point). An out-of-range alpha reaching cairo_paint_with_alpha puts
// the cairo_t into a permanent error state, silently dropping every later element
// in the frame, so the clamp must happen before the renderer ever sees it.
TEST(OverlayProtoMapperTest, OpacityAboveOneIsClampedToOne) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    element.mutable_style()->set_opacity(kAboveMaxOpacity);
    EXPECT_FLOAT_EQ(MapSingle(element).style.opacity, 1.0F);
}

TEST(OverlayProtoMapperTest, OpacityBelowZeroIsClampedToZero) {
    sst_cam::OverlayElement element;
    element.set_id("e");
    element.mutable_style()->set_opacity(kBelowMinOpacity);
    EXPECT_FLOAT_EQ(MapSingle(element).style.opacity, 0.0F);
}

}  // namespace
