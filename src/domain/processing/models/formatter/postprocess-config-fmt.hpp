#pragma once

#include <fmt/format.h>

#include "domain/common/models/formatter/pixel-format-fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/postprocess-config.hpp"

template <>
struct fmt::formatter<sst::processing::PostprocessConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::PostprocessConfig& cfg, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "PostprocessConfig{{\n"
                              "  output_width={},\n"
                              "  output_height={},\n"
                              "  output_format={}\n"
                              "}}",
                              cfg.output_width, cfg.output_height, cfg.output_format);
    }
};
