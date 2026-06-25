#pragma once

#include <fmt/format.h>

#include "domain/overlay/models/formatter/overlay-layout-fmt.hpp"  // IWYU pragma: keep
#include "domain/overlay/models/render-scene.hpp"

template <>
struct fmt::formatter<sst::overlay::RenderElement> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RenderElement& e, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RenderElement{{shape={}, text=\"{}\", {}}}", e.shape,
                              e.text, e.bounds);
    }
};

template <>
struct fmt::formatter<sst::overlay::RenderScene> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RenderScene& s, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RenderScene{{canvas={}x{}, elements={}}}", s.canvas_width,
                              s.canvas_height, s.elements.size());
    }
};

template <>
struct fmt::formatter<sst::overlay::RgbaImage> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RgbaImage& i, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RgbaImage{{{}x{} stride={} bytes={}}}", i.width, i.height,
                              i.stride, i.pixels.size());
    }
};
