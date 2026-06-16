#include "app/overlay/overlay-proto-mapper.hpp"

#include <algorithm>

namespace sst::overlay {

namespace {

auto MapShape(sst_cam::OverlayShape shape) -> OverlayShape {
    switch (shape) {
        case sst_cam::SHAPE_RECT:
            return OverlayShape::kRect;
        case sst_cam::SHAPE_TEXT:
            return OverlayShape::kText;
        case sst_cam::SHAPE_CIRCLE:
            return OverlayShape::kCircle;
        default:
            return OverlayShape::kUnknown;
    }
}

auto MapBinding(sst_cam::OverlayBinding binding) -> OverlayBinding {
    switch (binding) {
        case sst_cam::BINDING_SCORE_A:
            return OverlayBinding::kScoreA;
        case sst_cam::BINDING_SCORE_B:
            return OverlayBinding::kScoreB;
        case sst_cam::BINDING_SCORE_VS:
            return OverlayBinding::kScoreVs;
        case sst_cam::BINDING_TEAM_A_NAME:
            return OverlayBinding::kTeamAName;
        case sst_cam::BINDING_TEAM_B_NAME:
            return OverlayBinding::kTeamBName;
        case sst_cam::BINDING_MATCH_CLOCK:
            return OverlayBinding::kMatchClock;
        case sst_cam::BINDING_PERIOD_LABEL:
            return OverlayBinding::kPeriodLabel;
        default:
            return OverlayBinding::kStatic;
    }
}

auto MapAlign(sst_cam::TextAlign align) -> TextAlign {
    switch (align) {
        case sst_cam::TEXT_ALIGN_CENTER:
            return TextAlign::kCenter;
        case sst_cam::TEXT_ALIGN_RIGHT:
            return TextAlign::kRight;
        default:
            return TextAlign::kLeft;
    }
}

auto MapWeight(sst_cam::FontWeight weight) -> FontWeight {
    return weight == sst_cam::FONT_WEIGHT_BOLD ? FontWeight::kBold : FontWeight::kNormal;
}

auto MapRect(const sst_cam::OverlayRect& rect) -> OverlayRect {
    return OverlayRect{
        .x1 = rect.x1(), .y1 = rect.y1(), .x2 = rect.x2(), .y2 = rect.y2(), .z = rect.z()};
}

auto MapStyle(const sst_cam::OverlayStyle& style) -> OverlayStyle {
    // proto3 `optional`: absent opacity => documented default 1.0 (opaque), not
    // the scalar zero-value (0.0 = fully transparent). See overlay-rendering.md
    // "Element defaults". Clamp to [0,1] here, the single proto->domain
    // translation point: an out-of-range alpha reaching cairo_paint_with_alpha
    // puts the cairo_t into a permanent error state, silently dropping every
    // later element in the frame.
    const float opacity = style.has_opacity() ? style.opacity() : 1.0F;
    return OverlayStyle{
        .fill_color = style.fill_color(),
        .text_color = style.text_color(),
        .opacity = std::clamp(opacity, 0.0F, 1.0F),
        .corner_radius = style.corner_radius(),
        .font_family = style.font_family(),
        .font_size = style.font_size(),
        .text_align = MapAlign(style.text_align()),
        .font_weight = MapWeight(style.font_weight()),
        .static_text = style.static_text(),
    };
}

auto MapElement(const sst_cam::OverlayElement& element) -> OverlayElement {
    return OverlayElement{
        .id = element.id(),
        .shape = MapShape(element.shape()),
        .bounds = MapRect(element.bounds()),
        .style = MapStyle(element.style()),
        .binding = MapBinding(element.binding()),
        // proto3 `optional`: absent visible => documented default true (renders),
        // not the scalar zero-value (false = hidden). See overlay-rendering.md
        // "Element defaults".
        .visible = element.has_visible() ? element.visible() : true,
    };
}

}  // namespace

auto MapLayoutFromProto(const sst_cam::OverlayLayout& proto) -> OverlayLayout {
    OverlayLayout layout;
    layout.canvas_width = proto.canvas_width();
    layout.canvas_height = proto.canvas_height();
    for (const auto& element : proto.elements()) {
        layout.elements.push_back(MapElement(element));
    }
    for (const auto& tmpl : proto.templates()) {
        OverlayTemplate domain_tmpl;
        domain_tmpl.event_type = tmpl.event_type();
        domain_tmpl.duration_ms = tmpl.duration_ms();
        for (const auto& element : tmpl.elements()) {
            domain_tmpl.elements.push_back(MapElement(element));
        }
        layout.templates.push_back(std::move(domain_tmpl));
    }
    return layout;
}

}  // namespace sst::overlay
