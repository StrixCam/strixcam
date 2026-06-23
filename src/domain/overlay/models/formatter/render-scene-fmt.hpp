#pragma once

#include <fmt/format.h>

#include "domain/overlay/models/formatter/overlay-layout-fmt.hpp"  // IWYU pragma: keep
#include "domain/overlay/models/render-scene.hpp"

template <>
struct fmt::formatter<sst::overlay::RenderElement> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RenderElement& elem, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RenderElement{{shape={}, text=\"{}\", {}}}", elem.shape,
                              elem.text, elem.bounds);
    }
};

template <>
struct fmt::formatter<sst::overlay::RenderScene> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RenderScene& scene, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RenderScene{{canvas={}x{}, elements={}}}",
                              scene.canvas_width, scene.canvas_height, scene.elements.size());
    }
};

template <>
struct fmt::formatter<sst::overlay::RgbaImage> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::RgbaImage& img, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "RgbaImage{{{}x{} stride={} bytes={}}}", img.width,
                              img.height, img.stride, img.pixels.size());
    }
};
