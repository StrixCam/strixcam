#pragma once

#include <fmt/format.h>

#include "color-mode-fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/preprocess-config.hpp"

template <>
struct fmt::formatter<sst::processing::PreprocessConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::PreprocessConfig& c, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "PreprocessConfig{{\n"
                              "  ai_width={},\n"
                              "  ai_height={},\n"
                              "  ai_color_mode={},\n"
                              "  binary_threshold={}\n"
                              "}}",
                              c.ai_width, c.ai_height, c.ai_color_mode,
                              static_cast<unsigned>(c.binary_threshold));
    }
};
