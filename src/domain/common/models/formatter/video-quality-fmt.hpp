#pragma once

#include <fmt/format.h>

#include "domain/common/models/video-quality.hpp"

template <>
struct fmt::formatter<sst::common::VideoQuality> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::common::VideoQuality& quality, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "VideoQuality{{{}x{}@{}}}", quality.width, quality.height,
                              quality.fps);
    }
};
