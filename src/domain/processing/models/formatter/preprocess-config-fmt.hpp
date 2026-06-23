#pragma once

#include <fmt/format.h>

#include "color-mode-fmt.hpp"  // IWYU pragma: keep
#include "domain/processing/models/preprocess-config.hpp"

template <>
struct fmt::formatter<sst::processing::PreprocessConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::processing::PreprocessConfig& cfg, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "PreprocessConfig{{\n"
                              "  ai_width={},\n"
                              "  ai_height={},\n"
                              "  ai_color_mode={},\n"
                              "  binary_threshold={}\n"
                              "}}",
                              cfg.ai_width, cfg.ai_height, cfg.ai_color_mode,
                              static_cast<unsigned>(cfg.binary_threshold));
    }
};
