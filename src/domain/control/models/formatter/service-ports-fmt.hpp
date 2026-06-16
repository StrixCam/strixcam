#pragma once

#include <fmt/format.h>

#include "domain/control/models/service-ports.hpp"

template <>
struct fmt::formatter<sst::control::PreviewPort> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::control::PreviewPort p, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "PreviewPort{{{}}}", p.value);
    }
};

template <>
struct fmt::formatter<sst::control::DownloadPort> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::control::DownloadPort p, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "DownloadPort{{{}}}", p.value);
    }
};
