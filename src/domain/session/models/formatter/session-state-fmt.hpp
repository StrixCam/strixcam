#pragma once

#include <fmt/format.h>

#include "domain/session/models/formatter/live-match-fmt.hpp"       // IWYU pragma: keep
#include "domain/session/models/formatter/session-config-fmt.hpp"   // IWYU pragma: keep
#include "domain/session/models/formatter/session-phase-fmt.hpp"    // IWYU pragma: keep
#include "domain/session/models/formatter/session-summary-fmt.hpp"  // IWYU pragma: keep
#include "domain/session/models/session-state.hpp"

template <>
struct fmt::formatter<sst::session::SessionState> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::session::SessionState& state, FormatContext& ctx) const {
        auto out = fmt::format_to(ctx.out(), "SessionState{{app_connected={}, wifi_group={}, ",
                                  state.app_connected, state.wifi_group_up ? "up" : "down");
        out = fmt::format_to(out, "session={}, ", state.phase);
        if (state.config) {
            out = fmt::format_to(out, "config={}, ", *state.config);
        } else {
            out = fmt::format_to(out, "config=<none>, ");
        }
        out = fmt::format_to(out, "match={}, elapsed={}s, ", state.match,
                             state.recording_elapsed_seconds);
        if (state.last_summary) {
            return fmt::format_to(out, "last={}}}", *state.last_summary);
        }
        return fmt::format_to(out, "last=<none>}}");
    }
};
