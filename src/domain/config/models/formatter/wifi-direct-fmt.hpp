#pragma once

#include <fmt/format.h>

#include "domain/config/models/wifi-direct.hpp"
#include "fmt-helper.hpp"

template <>
struct fmt::formatter<sst::config::WifiDirectData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::WifiDirectData& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;

        return fmt::format_to(ctx.out(),
                              "WifiDirectData{{\n"
                              "  enabled={},\n"
                              "  ssid={},\n"
                              "  passphrase={},\n"
                              "  channel={},\n"
                              "  ip_address={}\n"
                              "}}",
                              BoolOptToStr(data.enabled), StrOptToStr(data.ssid),
                              StrOptToStr(data.passphrase),
                              data.channel.has_value() ? std::to_string(*data.channel) : "null",
                              StrOptToStr(data.ip_address));
    }
};
