#pragma once

#include <fmt/format.h>

#include <cstdint>

#include "domain/streaming/models/platform-stream-config.hpp"

template <>
struct fmt::formatter<sst::streaming::PlatformStreamType> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::streaming::PlatformStreamType type, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}", static_cast<std::uint8_t>(type));
    }
};

template <>
struct fmt::formatter<sst::streaming::PlatformStreamCodec> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::streaming::PlatformStreamCodec codec, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}", static_cast<std::uint8_t>(codec));
    }
};

template <>
struct fmt::formatter<sst::streaming::PlatformStreamConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::streaming::PlatformStreamConfig& cfg, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "PlatformStreamConfig{{\n"
                              "  stream_id={},\n"
                              "  name=\"{}\",\n"
                              "  type={},\n"
                              "  url=\"{}\",\n"
                              "  stream_key=\"{}\",\n"
                              "  codec={},\n"
                              "  width={},\n"
                              "  height={},\n"
                              "  framerate={},\n"
                              "  bitrate_kbps={}\n"
                              "}}",
                              cfg.stream_id, cfg.name, cfg.type, cfg.url, cfg.stream_key, cfg.codec,
                              cfg.width, cfg.height, cfg.framerate, cfg.bitrate_kbps);
    }
};
