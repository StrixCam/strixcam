#include "adapters/overlay/cairo/cairo-overlay-renderer.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace sst::adapters::overlay {

namespace {

using sst::overlay::FontWeight;
using sst::overlay::OverlayShape;
using sst::overlay::RenderElement;
using sst::overlay::RenderScene;
using sst::overlay::TextAlign;

struct Rgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};
    bool valid{false};
};

// An element's axis-aligned box in user space: top-left origin plus extents.
// Grouping the four geometry doubles keeps callees from swapping them.
struct BoxGeom {
    double pos_x{0.0};
    double pos_y{0.0};
    double width{0.0};
    double height{0.0};
};

// Parse "#RRGGBB" (also tolerates "RRGGBB"). Returns valid=false on empty/bad.
auto ParseHexColor(const std::string& hex) -> Rgb {
    constexpr std::size_t kHexDigits = 6;  // "RRGGBB" without the leading '#'
    constexpr int kDecimalBase = 10;       // 'a'/'A' map to nibble value 10
    constexpr int kNibbleBits = 4;         // high nibble shifts left one hex digit
    std::string digits = hex;
    if (!digits.empty() && digits.front() == '#') {
        digits.erase(digits.begin());
    }
    if (digits.size() != kHexDigits) {
        return {};
    }
    auto hex_byte = [&](std::size_t off) -> int {
      auto nib = [](char chr) -> int {
        if (chr >= '0' && chr <= '9') {
          return chr - '0';
        }
        if (chr >= 'a' && chr <= 'f') {
          return chr - 'a' + kDecimalBase;
        }
        if (chr >= 'A' && chr <= 'F') {
          return chr - 'A' + kDecimalBase;
        }
        return -1;
      };
      const int high = nib(digits[off]);
      const int low = nib(digits[off + 1]);
      if (high < 0 || low < 0) {
        return -1;
      }
      return (high << kNibbleBits) | low;
    };
    constexpr std::size_t kRedOffset = 0;
    constexpr std::size_t kGreenOffset = 2;
    constexpr std::size_t kBlueOffset = 4;
    const int red = hex_byte(kRedOffset);
    const int green = hex_byte(kGreenOffset);
    const int blue = hex_byte(kBlueOffset);
    if (red < 0 || green < 0 || blue < 0) {
        return {};
    }
    constexpr double kMax = 255.0;
    return {static_cast<double>(red) / kMax, static_cast<double>(green) / kMax,
            static_cast<double>(blue) / kMax, true};
}

void RoundedRectPath(cairo_t* cairo, double pos_x, double pos_y, double width, double height,
                     double radius) {
    constexpr double kHalf = 2.0;  // halve each side to clamp the corner radius
    const double rad = std::min({radius, width / kHalf, height / kHalf});
    if (rad <= 0.0) {
        cairo_rectangle(cairo, pos_x, pos_y, width, height);
        return;
    }
    constexpr double kHalfPi = M_PI / 2.0;
    constexpr double kThreeHalfPi = 3.0 * kHalfPi;  // start angle of the top-left arc
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, pos_x + width - rad, pos_y + rad, rad, -kHalfPi, 0);
    cairo_arc(cairo, pos_x + width - rad, pos_y + height - rad, rad, 0, kHalfPi);
    cairo_arc(cairo, pos_x + rad, pos_y + height - rad, rad, kHalfPi, M_PI);
    cairo_arc(cairo, pos_x + rad, pos_y + rad, rad, M_PI, kThreeHalfPi);
    cairo_close_path(cairo);
}

// Map a requested font family onto a metrically-comparable logical family that
// both stacks ship (overlay-rendering.md "Text rendering"): empty / unknown ->
// sans-serif. The three logical families (sans-serif / serif / monospace) pass
// through; Pango resolves them to a concrete shipped face.
auto ResolveFontFamily(const std::string& family) -> std::string {
    return family.empty() ? "sans-serif" : family;
}

// Lay out and paint the element's glyphs with Pango, clipped to its bounds.
// Inside a group (needs_group) the glyphs are painted opaque and the group pop
// applies the element opacity; otherwise opacity is folded into the source alpha
// directly, matching the rect/circle path.
void DrawGlyphs(cairo_t* cairo, const RenderElement& elem, const Rgb& text_color,
                const BoxGeom& box, double opacity, bool needs_group) {
    const double width = box.width;
    const double height = box.height;
    // U3: clip glyphs to the bounds box so text taller/wider than `bounds`
    // is clipped (no overflow past the bottom edge), per the contract.
    if (width > 0.0 && height > 0.0) {
        cairo_save(cairo);
        cairo_rectangle(cairo, box.pos_x, box.pos_y, width, height);
        cairo_clip(cairo);
    }

    PangoLayout* layout = pango_cairo_create_layout(cairo);
    pango_layout_set_text(layout, elem.text.c_str(), -1);

    PangoFontDescription* desc = pango_font_description_new();
    const std::string family = ResolveFontFamily(elem.style.font_family);
    pango_font_description_set_family(desc, family.c_str());
    pango_font_description_set_weight(desc, elem.style.font_weight == FontWeight::kBold
                                                ? PANGO_WEIGHT_BOLD
                                                : PANGO_WEIGHT_NORMAL);
    if (elem.style.font_size > 0.0F) {
        pango_font_description_set_absolute_size(
            desc, static_cast<double>(elem.style.font_size) * PANGO_SCALE);
    }
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    if (width > 0.0) {
        pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
    }
    PangoAlignment align = PANGO_ALIGN_LEFT;
    if (elem.style.text_align == TextAlign::kCenter) {
        align = PANGO_ALIGN_CENTER;
    } else if (elem.style.text_align == TextAlign::kRight) {
        align = PANGO_ALIGN_RIGHT;
    }
    pango_layout_set_alignment(layout, align);

    // U5 baseline: Pango lays the first line's top at the move-to origin, so
    // its baseline sits exactly one ascent below the top of `bounds` —
    // top-aligned text block, per the contract. Inside a group the glyphs are
    // painted opaque and the group's pop applies the element opacity;
    // without a group (opaque, no-fill fast path) fold opacity into the
    // source alpha directly, matching the rect/circle path.
    if (needs_group) {
        cairo_set_source_rgb(cairo, text_color.r, text_color.g, text_color.b);
    } else {
        cairo_set_source_rgba(cairo, text_color.r, text_color.g, text_color.b, opacity);
    }
    cairo_move_to(cairo, box.pos_x, box.pos_y);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);

    if (width > 0.0 && height > 0.0) {
        cairo_restore(cairo);  // drop the text clip
    }
}

void DrawText(cairo_t* cairo, const RenderElement& elem) {
    const Rgb text_color = ParseHexColor(elem.style.text_color);
    const Rgb fill = ParseHexColor(elem.style.fill_color);  // background box (U4)
    const BoxGeom box{.pos_x = elem.bounds.x1,
                      .pos_y = elem.bounds.y1,
                      .width = elem.bounds.x2 - elem.bounds.x1,
                      .height = elem.bounds.y2 - elem.bounds.y1};
    const auto opacity = static_cast<double>(elem.style.opacity);

    const bool have_glyphs = text_color.valid && !elem.text.empty();
    // Nothing to paint: no background fill and no drawable glyphs.
    if (!fill.valid && !have_glyphs) {
        return;
    }

    // A push_group allocates a full offscreen surface (~8MB at 1080p) every
    // frame, so only pay for it when it actually buys correctness: it exists to
    // composite the fill+glyph unit at a single per-element alpha without
    // overlapping coverage double-darkening (U5). That only matters when the
    // element is translucent OR has a background box behind the glyphs. With a
    // fully-opaque, no-fill text element (the common scoreboard case) the glyphs
    // alone need no group — paint them directly with the opacity folded into the
    // source color, matching the rect/circle path.
    constexpr double kEps = 1.0 / 512.0;
    const bool needs_group = (opacity < 1.0 - kEps) || fill.valid;

    if (needs_group) {
        // Composite the whole element (background box + glyphs) as a single unit
        // at one opacity (overlay-rendering.md: opacity multiplies the element's
        // overall alpha, applied to fill + text together). A group lets us paint
        // the sub-parts opaque and apply the per-element alpha once on pop —
        // overlapping fill/glyph coverage does not double-darken (U5).
        cairo_push_group(cairo);
    }

    // U4: non-empty fill_color paints the bounds background box behind glyphs,
    // mirroring the SHAPE_RECT fill path. corner_radius applies as for rects.
    if (fill.valid && box.width > 0.0 && box.height > 0.0) {
        RoundedRectPath(cairo, box.pos_x, box.pos_y, box.width, box.height,
                        static_cast<double>(elem.style.corner_radius));
        cairo_set_source_rgb(cairo, fill.r, fill.g, fill.b);
        cairo_fill(cairo);
    }

    if (have_glyphs) {
        DrawGlyphs(cairo, elem, text_color, box, opacity, needs_group);
    }

    if (needs_group) {
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, opacity);
    }
}

void DrawElement(cairo_t* cairo, const RenderElement& elem) {
    constexpr double kHalf = 2.0;       // center / radius = half the bounds extent
    constexpr double kFullTurn = 2.0;   // a full circle spans 2*pi radians
    const double pos_x = elem.bounds.x1;
    const double pos_y = elem.bounds.y1;
    const double width = elem.bounds.x2 - elem.bounds.x1;
    const double height = elem.bounds.y2 - elem.bounds.y1;
    const auto opacity = static_cast<double>(elem.style.opacity);

    switch (elem.shape) {
        case OverlayShape::kRect: {
            const Rgb fill = ParseHexColor(elem.style.fill_color);
            if (fill.valid && width > 0.0 && height > 0.0) {
                RoundedRectPath(cairo, pos_x, pos_y, width, height,
                                static_cast<double>(elem.style.corner_radius));
                cairo_set_source_rgba(cairo, fill.r, fill.g, fill.b, opacity);
                cairo_fill(cairo);
            }
            break;
        }
        case OverlayShape::kCircle: {
            const Rgb fill = ParseHexColor(elem.style.fill_color);
            if (fill.valid && width > 0.0 && height > 0.0) {
                cairo_save(cairo);
                cairo_translate(cairo, pos_x + width / kHalf, pos_y + height / kHalf);
                cairo_scale(cairo, width / kHalf, height / kHalf);
                cairo_arc(cairo, 0, 0, 1.0, 0, kFullTurn * M_PI);
                cairo_restore(cairo);
                cairo_set_source_rgba(cairo, fill.r, fill.g, fill.b, opacity);
                cairo_fill(cairo);
            }
            break;
        }
        case OverlayShape::kText:
            DrawText(cairo, elem);
            break;
        case OverlayShape::kUnknown:
            break;
    }
}

}  // namespace

auto CairoOverlayRenderer::Render(const RenderScene& scene, std::uint32_t out_width,
                                  std::uint32_t out_height) -> sst::overlay::RgbaImage {
    sst::overlay::RgbaImage img;
    img.width = out_width;
    img.height = out_height;
    img.stride = out_width * 4U;
    img.pixels.assign(static_cast<std::size_t>(img.stride) * out_height, 0U);
    if (out_width == 0 || out_height == 0) {
        return img;
    }

    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, static_cast<int>(out_width), static_cast<int>(out_height));
    cairo_t* cairo = cairo_create(surface);

    // Canvas -> output mapping: a SINGLE uniform scale factor that preserves
    // aspect ratio (overlay-rendering.md: non-uniform x/y scaling is
    // non-conformant). When the output aspect differs from the canvas aspect we
    // letterbox — center the scaled canvas so circles stay circular and corner
    // radii stay unskewed. Every length (font_size, corner_radius, rect edges)
    // then scales by this same factor.
    constexpr double kHalf = 2.0;  // split the leftover letterbox space evenly
    const double scale_x =
        scene.canvas_width > 0 ? static_cast<double>(out_width) / scene.canvas_width : 1.0;
    const double scale_y =
        scene.canvas_height > 0 ? static_cast<double>(out_height) / scene.canvas_height : 1.0;
    const double scale = std::min(scale_x, scale_y);
    const double scaled_w = static_cast<double>(scene.canvas_width) * scale;
    const double scaled_h = static_cast<double>(scene.canvas_height) * scale;
    const double offset_x = (static_cast<double>(out_width) - scaled_w) / kHalf;
    const double offset_y = (static_cast<double>(out_height) - scaled_h) / kHalf;
    cairo_translate(cairo, offset_x, offset_y);
    cairo_scale(cairo, scale, scale);

    for (const auto& element : scene.elements) {
        DrawElement(cairo, element);
    }

    cairo_surface_flush(surface);

    // Cairo ARGB32 is premultiplied, native-endian uint32 (little-endian byte
    // order B,G,R,A). Convert to straight-alpha RGBA8888.
    constexpr std::size_t kBytesPerPixel = 4;  // RGBA / BGRA, 8 bits per channel
    // static: referenced inside the unpremul lambda below without an explicit
    // capture, so it must have static (not automatic) storage duration.
    static constexpr int kChannelMax = 255;  // saturated 8-bit channel value
    static constexpr int kRoundHalf = 2;     // +alpha/2 rounds the divide to nearest
    // Cairo's premultiplied native-endian (little-endian) byte order is B,G,R,A.
    constexpr std::size_t kSrcBlue = 0;
    constexpr std::size_t kSrcGreen = 1;
    constexpr std::size_t kSrcRed = 2;
    constexpr std::size_t kSrcAlpha = 3;
    // Straight-alpha output byte order is R,G,B,A.
    constexpr std::size_t kDstRed = 0;
    constexpr std::size_t kDstGreen = 1;
    constexpr std::size_t kDstBlue = 2;
    constexpr std::size_t kDstAlpha = 3;
    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int cairo_stride = cairo_image_surface_get_stride(surface);
    for (std::uint32_t row = 0; row < out_height; ++row) {
        const unsigned char* in_row = src + static_cast<std::size_t>(row) * cairo_stride;
        std::uint8_t* out_row = img.pixels.data() + static_cast<std::size_t>(row) * img.stride;
        for (std::uint32_t col = 0; col < out_width; ++col) {
            const std::size_t base = static_cast<std::size_t>(col) * kBytesPerPixel;
            const std::uint8_t blue = in_row[base + kSrcBlue];
            const std::uint8_t green = in_row[base + kSrcGreen];
            const std::uint8_t red = in_row[base + kSrcRed];
            const std::uint8_t alpha = in_row[base + kSrcAlpha];
            auto unpremul = [alpha](std::uint8_t val) -> std::uint8_t {
                if (alpha == 0) {
                    return 0;
                }
                const int straight =
                    (static_cast<int>(val) * kChannelMax + alpha / kRoundHalf) / alpha;
                return static_cast<std::uint8_t>(std::min(kChannelMax, straight));
            };
            out_row[base + kDstRed] = unpremul(red);
            out_row[base + kDstGreen] = unpremul(green);
            out_row[base + kDstBlue] = unpremul(blue);
            out_row[base + kDstAlpha] = alpha;
        }
    }

    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    return img;
}

}  // namespace sst::adapters::overlay
