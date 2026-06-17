#pragma once

#include <fmt/format.h>

#include "domain/control/models/correlation-id.hpp"

template <>
struct fmt::formatter<sst::control::CorrelationId> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::control::CorrelationId& cid, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "CorrelationId{{{}}}", cid.value);
    }
};
