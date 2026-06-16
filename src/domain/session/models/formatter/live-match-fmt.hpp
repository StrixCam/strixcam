#pragma once

#include <fmt/format.h>

#include "domain/session/models/live-match.hpp"

template <>
struct fmt::formatter<sst::session::LiveMatch> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::session::LiveMatch& match, FormatContext& ctx) const {
        return fmt::format_to(
            ctx.out(), "LiveMatch{{score={}-{}, period={}, clock={}s, running={}, segment={}}}",
            match.score_a, match.score_b, match.period, match.clock_seconds, match.clock_running,
            static_cast<int>(match.segment));
    }
};
