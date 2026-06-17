// Cairo/Pango RGBA renderer (U8, R16/R17 + style fidelity).
// Runs in-container (CPU image surface, no GPU/X). Shape + alpha assertions use
// no fonts; a text element is rendered only as a no-crash smoke check.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "adapters/overlay/cairo/cairo-overlay-renderer.hpp"
#include "domain/overlay/models/overlay-enums.hpp"
#include "domain/overlay/models/overlay-layout.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace {

using sst::adapters::overlay::CairoOverlayRenderer;
using sst::overlay::OverlayRect;
using sst::overlay::OverlayShape;
using sst::overlay::RenderElement;
using sst::overlay::RenderScene;
using sst::overlay::RgbaImage;

struct Px {
    std::uint8_t r, g, b, a;
};

auto At(const RgbaImage& img, std::uint32_t col, std::uint32_t row) -> Px {
    constexpr std::size_t kBytesPerPixel = 4;  // RGBA8888
    const std::uint8_t* pix = img.pixels.data() + static_cast<std::size_t>(row) * img.stride +
                              static_cast<std::size_t>(col) * kBytesPerPixel;
    return Px{pix[0], pix[1], pix[2], pix[3]};
}

// Top-left / bottom-right corner of a rect, grouped so the four coordinate
// floats can't be passed in the wrong order (bugprone-easily-swappable-params).
struct RectCorners {
    float x1;
    float y1;
    float x2;
    float y2;
};

// Fill styling for a test rect. Bundling the color string with the opacity and
// corner-radius floats keeps the two floats off adjacent parameter slots
// (bugprone-easily-swappable-parameters) and gives each a labeled call site.
struct FillStyle {
    std::string color;
    float opacity;
    float radius = 0.0F;
};

auto RectElement(const RectCorners& corners, const FillStyle& style) -> RenderElement {
    RenderElement elem;
    elem.shape = OverlayShape::kRect;
    elem.bounds =
        OverlayRect{.x1 = corners.x1, .y1 = corners.y1, .x2 = corners.x2, .y2 = corners.y2, .z = 1};
    elem.style.fill_color = style.color;
    elem.style.opacity = style.opacity;
    elem.style.corner_radius = style.radius;
    return elem;
}

// Channel thresholds for "this pixel is the saturated primary it should be":
// a primary channel reads near full and the other two near zero. Shared by the
// fill-color assertions across these tests.
constexpr std::uint8_t kChannelHigh = 200;  // a saturated channel reads >= this
constexpr std::uint8_t kChannelLow = 50;    // a zero channel reads <= this
constexpr std::uint8_t kOpaque = 255;       // fully opaque alpha
constexpr int kBytesPerPixelInt = 4;        // RGBA8888 stride multiplier

// R16/R17: a filled rect renders the fill color inside and transparent outside.
TEST(CairoRendererTest, RectFillAndTransparency) {
    constexpr std::uint32_t kDim = 100;  // square canvas == square output
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    scene.elements.push_back(
        RectElement({.x1 = 10, .y1 = 10, .x2 = 60, .y2 = 60},  // NOLINT(readability-magic-numbers)
                    {.color = "#FF0000", .opacity = 1.0F}));

    auto img = renderer.Render(scene, kDim, kDim);
    ASSERT_EQ(img.width, kDim);
    ASSERT_EQ(img.height, kDim);
    ASSERT_EQ(img.pixels.size(), static_cast<std::size_t>(kDim) * kDim * kBytesPerPixelInt);

    const Px inside = At(img, 30, 30);  // NOLINT(readability-magic-numbers) probe inside the rect
    EXPECT_GT(inside.r, kChannelHigh);
    EXPECT_LT(inside.g, kChannelLow);
    EXPECT_LT(inside.b, kChannelLow);
    EXPECT_EQ(inside.a, kOpaque);

    const Px outside = At(img, 90, 90);  // NOLINT(readability-magic-numbers) probe outside the rect
    EXPECT_EQ(outside.a, 0);             // transparent where nothing was drawn
}

// Output scales canvas -> output resolution (a 2x render keeps the fill).
TEST(CairoRendererTest, ScalesCanvasToOutput) {
    constexpr std::uint32_t kCanvas = 100;
    constexpr std::uint32_t kOutput = 200;  // 2x upscale
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kCanvas;
    scene.canvas_height = kCanvas;
    scene.elements.push_back(
        RectElement({.x1 = 0, .y1 = 0, .x2 = 100, .y2 = 100},  // NOLINT(readability-magic-numbers)
                    {.color = "#00FF00", .opacity = 1.0F}));

    auto img = renderer.Render(scene, kOutput, kOutput);
    ASSERT_EQ(img.width, kOutput);
    const Px pix = At(img, 100, 100);  // NOLINT(readability-magic-numbers) center of 200x200 output
    EXPECT_GT(pix.g, kChannelHigh);
    EXPECT_EQ(pix.a, kOpaque);
}

// Opacity is honored (half-opacity fill -> alpha near 127).
TEST(CairoRendererTest, OpacityHonored) {
    constexpr std::uint32_t kDim = 50;
    constexpr float kHalfOpacity = 0.5F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    scene.elements.push_back(
        RectElement({.x1 = 0, .y1 = 0, .x2 = 50, .y2 = 50},  // NOLINT(readability-magic-numbers)
                    {.color = "#FFFFFF", .opacity = kHalfOpacity}));

    auto img = renderer.Render(scene, kDim, kDim);
    const Px pix = At(img, 25, 25);  // NOLINT(readability-magic-numbers) center of 50x50
    // Half opacity over a transparent surface lands near mid-alpha (255/2).
    EXPECT_NEAR(pix.a, 127, 12);  // NOLINT(readability-magic-numbers) ~50% alpha, tolerant band
}

// Corner radius rounds the rect: the extreme corner pixel is transparent while
// the center is filled.
TEST(CairoRendererTest, CornerRadiusRoundsCorners) {
    constexpr std::uint32_t kDim = 100;
    constexpr float kRadius = 30.0F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    scene.elements.push_back(
        RectElement({.x1 = 0, .y1 = 0, .x2 = 100, .y2 = 100},  // NOLINT(readability-magic-numbers)
                    {.color = "#FFFFFF", .opacity = 1.0F, .radius = kRadius}));

    auto img = renderer.Render(scene, kDim, kDim);
    EXPECT_EQ(At(img, 0, 0).a, 0);          // corner cut away
    EXPECT_EQ(At(img, 50, 50).a, kOpaque);  // NOLINT(readability-magic-numbers) center filled
}

// Circle fills its center but leaves the bounding-box corners transparent.
TEST(CairoRendererTest, CircleInscribedInBounds) {
    constexpr std::uint32_t kDim = 100;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    RenderElement elem;
    elem.shape = OverlayShape::kCircle;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 100, .y2 = 100, .z = 1};
    elem.style.fill_color = "#0000FF";
    elem.style.opacity = 1.0F;
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kDim, kDim);
    EXPECT_EQ(At(img, 50, 50).a,
              kOpaque);             // NOLINT(readability-magic-numbers) center inside circle
    EXPECT_EQ(At(img, 2, 2).a, 0);  // NOLINT(readability-magic-numbers) bbox corner outside circle
}

// Empty scene -> fully transparent surface.
TEST(CairoRendererTest, EmptySceneIsTransparent) {
    constexpr std::uint32_t kDim = 20;
    constexpr std::size_t kAlphaOffset = 3;  // alpha is the 4th byte of each RGBA pixel
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    auto img = renderer.Render(scene, kDim, kDim);
    for (std::size_t idx = kAlphaOffset; idx < img.pixels.size(); idx += kBytesPerPixelInt) {
        EXPECT_EQ(img.pixels[idx], 0);  // every alpha byte is 0
    }
}

// U2: an off-aspect output (wide) keeps a circle inscribed in square canvas
// bounds circular — a uniform scale + letterbox, never an x/y stretch. A
// non-uniform scale would stretch the circle into a wide ellipse, lighting up
// pixels far left/right of center that the inscribed circle never covers.
TEST(CairoRendererTest, UniformScalePreservesCircleOffAspect) {
    constexpr std::uint32_t kCanvas = 100;
    constexpr std::uint32_t kOutWidth = 200;  // off-aspect: twice the height
    constexpr std::uint32_t kOutHeight = 100;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kCanvas;
    scene.canvas_height = kCanvas;
    RenderElement elem;
    elem.shape = OverlayShape::kCircle;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 100, .y2 = 100, .z = 1};
    elem.style.fill_color = "#00FF00";
    elem.style.opacity = 1.0F;
    scene.elements.push_back(elem);

    // 200x100 output: scale = min(2, 1) = 1, canvas centered horizontally with
    // 50px letterbox bars on each side. A 100px circle inscribed -> radius 50,
    // center at (100, 50).
    auto img = renderer.Render(scene, kOutWidth, kOutHeight);
    // NOLINTNEXTLINE(readability-magic-numbers) circle center filled
    EXPECT_EQ(At(img, 100, 50).a, kOpaque);
    // If the canvas had been x-stretched to fill 200px width, these far columns
    // would be inside the ellipse; with uniform scale + centering they are in
    // the letterbox and stay transparent.
    EXPECT_EQ(At(img, 5, 50).a, 0);    // NOLINT(readability-magic-numbers) left letterbox bar
    EXPECT_EQ(At(img, 195, 50).a, 0);  // NOLINT(readability-magic-numbers) right letterbox bar
    // The circle's own bbox corner (within the centered canvas) is outside it.
    EXPECT_EQ(At(img, 52, 2).a, 0);  // NOLINT(readability-magic-numbers) bbox corner
}

// U3: text taller than its bounds is clipped — no painted pixels below the
// bounds bottom edge. Multi-line text with a tiny-height box forces overflow.
TEST(CairoRendererTest, TextClippedToBoundsHeight) {
    constexpr std::uint32_t kDim = 200;
    constexpr float kBoxHeight = 24.0F;       // box too short to hold three lines
    constexpr float kFontSize = 40.0F;        // a line taller than the box
    constexpr std::uint32_t kBelowClip = 30;  // first scanned row, safely past the box bottom
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kDim;
    scene.canvas_height = kDim;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // A short box (24px tall) that cannot hold three 40px lines.
    // NOLINTNEXTLINE(readability-magic-numbers) full-width, short-height bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 200, .y2 = kBoxHeight, .z = 1};
    elem.style.text_color = "#FFFFFF";
    elem.style.font_size = kFontSize;
    elem.style.opacity = 1.0F;
    elem.text = "AAAA\nBBBB\nCCCC";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kDim, kDim);
    // No glyph pixel may appear below the bounds bottom edge (y >= 24).
    for (std::uint32_t row = kBelowClip; row < kDim; ++row) {
        for (std::uint32_t col = 0; col < kDim; ++col) {
            EXPECT_EQ(At(img, col, row).a, 0)
                << "painted pixel below clip at (" << col << "," << row << ")";
        }
    }
}

// U4: a TEXT element with non-empty fill_color paints the bounds background box
// behind the glyphs — assert background pixels inside bounds carry the fill.
TEST(CairoRendererTest, TextFillColorPaintsBackgroundBox) {
    constexpr std::uint32_t kWidth = 120;
    constexpr std::uint32_t kHeight = 60;
    constexpr float kFontSize = 20.0F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 120, .y2 = 60, .z = 1};
    elem.style.fill_color = "#FF0000";  // red background box
    elem.style.text_color = "#FFFFFF";  // white glyphs
    elem.style.font_size = kFontSize;
    elem.style.opacity = 1.0F;
    elem.text = "Hi";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    // A bottom corner inside bounds is background, not a glyph -> red, opaque.
    const Px bgpx = At(img, 110, 55);  // NOLINT(readability-magic-numbers) bottom-right background
    EXPECT_GT(bgpx.r, kChannelHigh);
    EXPECT_LT(bgpx.g, kChannelLow);
    EXPECT_LT(bgpx.b, kChannelLow);
    EXPECT_EQ(bgpx.a, kOpaque);
}

// U4 edge: empty fill_color paints no background — inside-bounds pixels with no
// glyph stay fully transparent.
TEST(CairoRendererTest, TextWithoutFillHasTransparentBackground) {
    constexpr std::uint32_t kWidth = 120;
    constexpr std::uint32_t kHeight = 60;
    constexpr float kFontSize = 20.0F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 120, .y2 = 60, .z = 1};
    elem.style.fill_color = "";  // no background box
    elem.style.text_color = "#FFFFFF";
    elem.style.font_size = kFontSize;
    elem.style.opacity = 1.0F;
    elem.text = "Hi";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    // NOLINTNEXTLINE(readability-magic-numbers) bottom corner, no glyph, no fill
    EXPECT_EQ(At(img, 110, 55).a, 0);
}

// U5: per-element opacity composites the fill+text unit at the given alpha —
// a 0.5-opacity red background box yields ~50% alpha pixels (group compositing,
// not double-darkening).
TEST(CairoRendererTest, TextElementOpacityCompositesAsUnit) {
    constexpr std::uint32_t kWidth = 120;
    constexpr std::uint32_t kHeight = 60;
    constexpr float kFontSize = 20.0F;
    constexpr float kHalfOpacity = 0.5F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 120, .y2 = 60, .z = 1};
    elem.style.fill_color = "#FF0000";
    elem.style.text_color = "#FFFFFF";
    elem.style.font_size = kFontSize;
    elem.style.opacity = kHalfOpacity;
    elem.text = "Hi";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    const Px bgpx = At(img, 110, 55);  // NOLINT(readability-magic-numbers) background-only pixel
    EXPECT_NEAR(bgpx.a, 127, 12);     // NOLINT(readability-magic-numbers) ~50% alpha, tolerant band
    EXPECT_GT(bgpx.r, kChannelHigh);  // straight-alpha red preserved
    EXPECT_LT(bgpx.g, kChannelLow);
}

// U5 edge: empty font family falls back to a shipped comparable face without
// crashing (smoke; in-container font availability is limited).
TEST(CairoRendererTest, EmptyFontFamilyFallsBack) {
    constexpr std::uint32_t kWidth = 200;
    constexpr std::uint32_t kHeight = 60;
    constexpr float kFontSize = 24.0F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 200, .y2 = 60, .z = 1};
    elem.style.text_color = "#FFFFFF";
    elem.style.font_family = "";  // unset -> sans-serif fallback
    elem.style.font_size = kFontSize;
    elem.style.opacity = 1.0F;
    elem.text = "Score";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    EXPECT_EQ(img.width, kWidth);
    EXPECT_EQ(img.height, kHeight);
}

// #12: the opaque, no-fill text element takes the no-push_group fast path
// (opacity folded into the glyph source color). Glyphs must still paint at full
// alpha and nothing must leak outside the bounds — behavior identical to the
// grouped path. A large white block-glyph string on a tall box guarantees some
// fully-opaque glyph pixel exists.
TEST(CairoRendererTest, OpaqueNoFillTextPaintsGlyphsAtFullAlpha) {
    CairoOverlayRenderer renderer;
    RenderScene scene;
    constexpr std::uint32_t kWidth = 200;
    constexpr std::uint32_t kHeight = 80;
    constexpr float kFontSize = 48.0F;  // large block glyphs guarantee opaque coverage
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 200, .y2 = 80, .z = 1};
    elem.style.fill_color = "";         // no background -> no group
    elem.style.text_color = "#FFFFFF";  // opaque white glyphs
    elem.style.font_size = kFontSize;
    elem.style.opacity = 1.0F;  // fully opaque -> no group
    elem.text = "MMMM";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    std::uint8_t max_alpha = 0;
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        for (std::uint32_t col = 0; col < kWidth; ++col) {
            max_alpha = std::max(max_alpha, At(img, col, row).a);
        }
    }
    EXPECT_EQ(max_alpha, kOpaque) << "opaque no-fill text should paint full-alpha glyph pixels";
}

// Text element renders without crashing (font availability aside).
TEST(CairoRendererTest, TextRenderDoesNotCrash) {
    constexpr std::uint32_t kWidth = 200;
    constexpr std::uint32_t kHeight = 60;
    constexpr float kFontSize = 32.0F;
    CairoOverlayRenderer renderer;
    RenderScene scene;
    scene.canvas_width = kWidth;
    scene.canvas_height = kHeight;
    RenderElement elem;
    elem.shape = OverlayShape::kText;
    // NOLINTNEXTLINE(readability-magic-numbers) full-canvas bounds
    elem.bounds = OverlayRect{.x1 = 0, .y1 = 0, .x2 = 200, .y2 = 60, .z = 1};
    elem.style.text_color = "#FFFFFF";
    elem.style.font_size = kFontSize;
    elem.text = "2 - 1";
    scene.elements.push_back(elem);

    auto img = renderer.Render(scene, kWidth, kHeight);
    EXPECT_EQ(img.width, kWidth);
    EXPECT_EQ(img.height, kHeight);
}

}  // namespace
