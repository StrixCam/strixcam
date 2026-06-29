#pragma once

#include <fmt/format.h>

#include "domain/streaming/models/preview-layout.hpp"

template <>
struct fmt::formatter<sst::streaming::PreviewLayout> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(sst::streaming::PreviewLayout layout, Ctx& ctx) const {
        const auto* name =
            layout == sst::streaming::PreviewLayout::kSideBySide ? "side_by_side" : "single";
        return fmt::format_to(ctx.out(), "{}", name);
    }
};

template <>
struct fmt::formatter<sst::streaming::PreviewLayoutState> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::streaming::PreviewLayoutState& state, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "PreviewLayoutState{{{}}}", state.Get());
    }
};
