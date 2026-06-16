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

// Parse "#RRGGBB" (also tolerates "RRGGBB"). Returns valid=false on empty/bad.
auto ParseHexColor(const std::string& hex) -> Rgb {
    std::string s = hex;
    if (!s.empty() && s.front() == '#') {
        s.erase(s.begin());
    }
    if (s.size() != 6) {
        return {};
    }
    auto hex_byte = [&](std::size_t off) -> int {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') {
          return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
          return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
          return c - 'A' + 10;
        }
        return -1;
      };
      const int hi = nib(s[off]);
      const int lo = nib(s[off + 1]);
      if (hi < 0 || lo < 0) {
        return -1;
      }
      return (hi << 4) | lo;
    };
    const int r = hex_byte(0);
    const int g = hex_byte(2);
    const int b = hex_byte(4);
    if (r < 0 || g < 0 || b < 0) {
        return {};
    }
    constexpr double kMax = 255.0;
    return {static_cast<double>(r) / kMax, static_cast<double>(g) / kMax,
            static_cast<double>(b) / kMax, true};
}

void RoundedRectPath(cairo_t* cr, double x, double y, double w, double h, double radius) {
    const double r = std::min({radius, w / 2.0, h / 2.0});
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    constexpr double kHalfPi = M_PI / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -kHalfPi, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, kHalfPi);
    cairo_arc(cr, x + r, y + h - r, r, kHalfPi, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * kHalfPi);
    cairo_close_path(cr);
}

// Map a requested font family onto a metrically-comparable logical family that
// both stacks ship (overlay-rendering.md "Text rendering"): empty / unknown ->
// sans-serif. The three logical families (sans-serif / serif / monospace) pass
// through; Pango resolves them to a concrete shipped face.
auto ResolveFontFamily(const std::string& family) -> std::string {
    return family.empty() ? "sans-serif" : family;
}

void DrawText(cairo_t* cr, const RenderElement& e) {
    const Rgb text_color = ParseHexColor(e.style.text_color);
    const Rgb fill = ParseHexColor(e.style.fill_color);  // background box (U4)
    const double x = e.bounds.x1;
    const double y = e.bounds.y1;
    const double w = e.bounds.x2 - e.bounds.x1;
    const double h = e.bounds.y2 - e.bounds.y1;
    const auto opacity = static_cast<double>(e.style.opacity);

    const bool have_glyphs = text_color.valid && !e.text.empty();
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
        cairo_push_group(cr);
    }

    // U4: non-empty fill_color paints the bounds background box behind glyphs,
    // mirroring the SHAPE_RECT fill path. corner_radius applies as for rects.
    if (fill.valid && w > 0.0 && h > 0.0) {
        RoundedRectPath(cr, x, y, w, h, static_cast<double>(e.style.corner_radius));
        cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
        cairo_fill(cr);
    }

    if (have_glyphs) {
        // U3: clip glyphs to the bounds box so text taller/wider than `bounds`
        // is clipped (no overflow past the bottom edge), per the contract.
        if (w > 0.0 && h > 0.0) {
            cairo_save(cr);
            cairo_rectangle(cr, x, y, w, h);
            cairo_clip(cr);
        }

        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, e.text.c_str(), -1);

        PangoFontDescription* desc = pango_font_description_new();
        const std::string family = ResolveFontFamily(e.style.font_family);
        pango_font_description_set_family(desc, family.c_str());
        pango_font_description_set_weight(desc, e.style.font_weight == FontWeight::kBold
                                                    ? PANGO_WEIGHT_BOLD
                                                    : PANGO_WEIGHT_NORMAL);
        if (e.style.font_size > 0.0F) {
            pango_font_description_set_absolute_size(
                desc, static_cast<double>(e.style.font_size) * PANGO_SCALE);
        }
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        if (w > 0.0) {
            pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
        }
        PangoAlignment align = PANGO_ALIGN_LEFT;
        if (e.style.text_align == TextAlign::kCenter) {
            align = PANGO_ALIGN_CENTER;
        } else if (e.style.text_align == TextAlign::kRight) {
            align = PANGO_ALIGN_RIGHT;
        }
        pango_layout_set_alignment(layout, align);

        // U5 baseline: Pango lays the first line's top at the move-to origin, so
        // its baseline sits exactly one ascent below the top of `bounds` —
        // top-aligned text block, per the contract. Inside a group the glyphs are
        // painted opaque and the group's pop applies the element opacity;
        // without a group (opaque, no-fill fast path) fold opacity into the
        // source alpha directly, matching the rect/circle path.
        if (text_color.valid) {
            if (needs_group) {
                cairo_set_source_rgb(cr, text_color.r, text_color.g, text_color.b);
            } else {
                cairo_set_source_rgba(cr, text_color.r, text_color.g, text_color.b, opacity);
            }
        }
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        if (w > 0.0 && h > 0.0) {
            cairo_restore(cr);  // drop the text clip
        }
    }

    if (needs_group) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, opacity);
    }
}

void DrawElement(cairo_t* cr, const RenderElement& e) {
    const double x = e.bounds.x1;
    const double y = e.bounds.y1;
    const double w = e.bounds.x2 - e.bounds.x1;
    const double h = e.bounds.y2 - e.bounds.y1;
    const auto opacity = static_cast<double>(e.style.opacity);

    switch (e.shape) {
        case OverlayShape::kRect: {
            const Rgb fill = ParseHexColor(e.style.fill_color);
            if (fill.valid && w > 0.0 && h > 0.0) {
                RoundedRectPath(cr, x, y, w, h, static_cast<double>(e.style.corner_radius));
                cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, opacity);
                cairo_fill(cr);
            }
            break;
        }
        case OverlayShape::kCircle: {
            const Rgb fill = ParseHexColor(e.style.fill_color);
            if (fill.valid && w > 0.0 && h > 0.0) {
                cairo_save(cr);
                cairo_translate(cr, x + w / 2.0, y + h / 2.0);
                cairo_scale(cr, w / 2.0, h / 2.0);
                cairo_arc(cr, 0, 0, 1.0, 0, 2.0 * M_PI);
                cairo_restore(cr);
                cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, opacity);
                cairo_fill(cr);
            }
            break;
        }
        case OverlayShape::kText:
            DrawText(cr, e);
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
    cairo_t* cr = cairo_create(surface);

    // Canvas -> output mapping: a SINGLE uniform scale factor that preserves
    // aspect ratio (overlay-rendering.md: non-uniform x/y scaling is
    // non-conformant). When the output aspect differs from the canvas aspect we
    // letterbox — center the scaled canvas so circles stay circular and corner
    // radii stay unskewed. Every length (font_size, corner_radius, rect edges)
    // then scales by this same factor.
    const double sx =
        scene.canvas_width > 0 ? static_cast<double>(out_width) / scene.canvas_width : 1.0;
    const double sy =
        scene.canvas_height > 0 ? static_cast<double>(out_height) / scene.canvas_height : 1.0;
    const double scale = std::min(sx, sy);
    const double scaled_w = static_cast<double>(scene.canvas_width) * scale;
    const double scaled_h = static_cast<double>(scene.canvas_height) * scale;
    const double offset_x = (static_cast<double>(out_width) - scaled_w) / 2.0;
    const double offset_y = (static_cast<double>(out_height) - scaled_h) / 2.0;
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);

    for (const auto& element : scene.elements) {
        DrawElement(cr, element);
    }

    cairo_surface_flush(surface);

    // Cairo ARGB32 is premultiplied, native-endian uint32 (little-endian byte
    // order B,G,R,A). Convert to straight-alpha RGBA8888.
    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int cairo_stride = cairo_image_surface_get_stride(surface);
    for (std::uint32_t row = 0; row < out_height; ++row) {
        const unsigned char* in = src + static_cast<std::size_t>(row) * cairo_stride;
        std::uint8_t* out = img.pixels.data() + static_cast<std::size_t>(row) * img.stride;
        for (std::uint32_t col = 0; col < out_width; ++col) {
            const std::uint8_t b = in[col * 4 + 0];
            const std::uint8_t g = in[col * 4 + 1];
            const std::uint8_t r = in[col * 4 + 2];
            const std::uint8_t a = in[col * 4 + 3];
            auto unpremul = [a](std::uint8_t v) -> std::uint8_t {
                if (a == 0) {
                    return 0;
                }
                const int s = (static_cast<int>(v) * 255 + a / 2) / a;
                return static_cast<std::uint8_t>(std::min(255, s));
            };
            out[col * 4 + 0] = unpremul(r);
            out[col * 4 + 1] = unpremul(g);
            out[col * 4 + 2] = unpremul(b);
            out[col * 4 + 3] = a;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return img;
}

}  // namespace sst::adapters::overlay
