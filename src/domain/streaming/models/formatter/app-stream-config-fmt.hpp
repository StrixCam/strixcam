#pragma once

#include <fmt/format.h>

#include "domain/streaming/models/app-stream-config.hpp"

template <>
struct fmt::formatter<sst::streaming::AppStreamConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::streaming::AppStreamConfig& cfg, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "AppStreamConfig{{\n"
                              "  mount_point=\"{}\",\n"
                              "  port={},\n"
                              "  width={},\n"
                              "  height={},\n"
                              "  framerate={},\n"
                              "  bitrate_kbps={}\n"
                              "}}",
                              cfg.mount_point, cfg.port, cfg.width, cfg.height, cfg.framerate,
                              cfg.bitrate_kbps);
    }
};
