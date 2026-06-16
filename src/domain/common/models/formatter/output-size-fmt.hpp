#pragma once

#include <fmt/format.h>

#include "domain/common/models/output-size.hpp"

template <>
struct fmt::formatter<sst::common::OutputSize> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::common::OutputSize& s, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "OutputSize{{width={}, height={}}}", s.width, s.height);
    }
};
