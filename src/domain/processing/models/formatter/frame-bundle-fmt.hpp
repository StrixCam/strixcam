#pragma once

#include <fmt/format.h>

#include "domain/common/models/formatter/pixel-format-fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/frame-bundle.hpp"

template <>
struct fmt::formatter<sst::processing::FrameBundle> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::FrameBundle& b, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "FrameBundle{{src={}x{} {} id={}, ai={}x{} {}}}",
                              b.source_frame.geometry.width, b.source_frame.geometry.height,
                              b.source_frame.format, b.source_frame.frame_id,
                              b.ai_frame.geometry.width, b.ai_frame.geometry.height,
                              b.ai_frame.format);
    }
};
