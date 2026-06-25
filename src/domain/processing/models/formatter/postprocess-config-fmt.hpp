#pragma once

#include <fmt/format.h>

#include "domain/common/models/formatter/pixel-format-fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/postprocess-config.hpp"

template <>
struct fmt::formatter<sst::processing::PostprocessConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::PostprocessConfig& c, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "PostprocessConfig{{\n"
                              "  output_width={},\n"
                              "  output_height={},\n"
                              "  output_format={}\n"
                              "}}",
                              c.output_width, c.output_height, c.output_format);
    }
};
