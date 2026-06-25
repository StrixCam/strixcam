#pragma once

#include <fmt/format.h>

#include "domain/processing/models/crop-rect.hpp"

template <>
struct fmt::formatter<sst::processing::CropRect> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::CropRect& r, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "CropRect{{x={}, y={}, w={}, h={}}}", r.x, r.y, r.width,
                              r.height);
    }
};
