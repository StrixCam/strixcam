#pragma once

#include <fmt/format.h>

#include "domain/overlay/models/formatter/overlay-enums-fmt.hpp"  // IWYU pragma: keep
#include "domain/overlay/models/overlay-layout.hpp"

template <>
struct fmt::formatter<sst::overlay::OverlayRect> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayRect& r, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Rect{{({},{})-({},{}) z={}}}", r.x1, r.y1, r.x2, r.y2,
                              r.z);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayStyle> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayStyle& s, Ctx& ctx) const {
        return fmt::format_to(ctx.out(),
                              "Style{{fill={}, text={}, opacity={}, radius={}, font={}@{}, "
                              "align={}, weight={}, static=\"{}\"}}",
                              s.fill_color, s.text_color, s.opacity, s.corner_radius, s.font_family,
                              s.font_size, s.text_align, s.font_weight, s.static_text);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayElement> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayElement& e, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Element{{id={}, shape={}, binding={}, visible={}, {}}}",
                              e.id, e.shape, e.binding, e.visible, e.bounds);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayTemplate> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayTemplate& t, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Template{{event={}, duration_ms={}, elements={}}}",
                              t.event_type, t.duration_ms, t.elements.size());
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayLayout> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayLayout& l, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "Layout{{canvas={}x{}, elements={}, templates={}}}",
                              l.canvas_width, l.canvas_height, l.elements.size(),
                              l.templates.size());
    }
};
