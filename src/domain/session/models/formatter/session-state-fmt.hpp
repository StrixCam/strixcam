#pragma once

#include <fmt/format.h>

#include "domain/session/models/formatter/live-match-fmt.hpp"      // IWYU pragma: keep
#include "domain/session/models/formatter/session-config-fmt.hpp"  // IWYU pragma: keep
#include "domain/session/models/formatter/session-phase-fmt.hpp"   // IWYU pragma: keep
#include "domain/session/models/session-state.hpp"

template <>
struct fmt::formatter<sst::session::SessionState> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::session::SessionState& state, FormatContext& ctx) const {
        auto out = fmt::format_to(ctx.out(), "SessionState{{phase={}, ", state.phase);
        if (state.config) {
            out = fmt::format_to(out, "config={}, ", *state.config);
        } else {
            out = fmt::format_to(out, "config=<none>, ");
        }
        return fmt::format_to(out, "match={}}}", state.match);
    }
};
