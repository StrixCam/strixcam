#pragma once

#include <fmt/format.h>

#include "domain/overlay/models/formatter/overlay-enums-fmt.hpp"  // IWYU pragma: keep
#include "domain/overlay/models/overlay-layout.hpp"

template <>
struct fmt::formatter<sst::overlay::OverlayRect> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayRect& rect, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Rect{{({},{})-({},{}) z={}}}", rect.x1, rect.y1, rect.x2,
                              rect.y2, rect.z);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayStyle> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayStyle& style, Ctx& ctx) const {
        return fmt::format_to(ctx.out(),
                              "Style{{fill={}, text={}, opacity={}, radius={}, font={}@{}, "
                              "align={}, weight={}, static=\"{}\"}}",
                              style.fill_color, style.text_color, style.opacity,
                              style.corner_radius, style.font_family, style.font_size,
                              style.text_align, style.font_weight, style.static_text);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayElement> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayElement& elem, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Element{{id={}, shape={}, binding={}, visible={}, {}}}",
                              elem.id, elem.shape, elem.binding, elem.visible, elem.bounds);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayTemplate> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayTemplate& tmpl, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Template{{event={}, duration_ms={}, elements={}}}",
                              tmpl.event_type, tmpl.duration_ms, tmpl.elements.size());
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayLayout> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayLayout& layout, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Layout{{canvas={}x{}, elements={}, templates={}}}",
                              layout.canvas_width, layout.canvas_height, layout.elements.size(),
                              layout.templates.size());
    }
};
