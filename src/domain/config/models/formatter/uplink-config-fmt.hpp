#pragma once

#include <fmt/format.h>

#include "domain/config/models/uplink-config.hpp"
#include "fmt-helper.hpp"

template <>
struct fmt::formatter<sst::config::EthernetUplink> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::EthernetUplink& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;
        return fmt::format_to(
            ctx.out(), "EthernetUplink{{enabled={}, dhcp={}, address={}, gateway={}, dns={}}}",
            BoolOptToStr(data.enabled), BoolOptToStr(data.dhcp), StrOptToStr(data.address),
            StrOptToStr(data.gateway), StrOptToStr(data.dns));
    }
};

template <>
struct fmt::formatter<sst::config::WifiStaUplink> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::WifiStaUplink& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;
        return fmt::format_to(
            ctx.out(),
            "WifiStaUplink{{enabled={}, ssid={}, passphrase={}, dhcp={}, address={}, "
            "gateway={}, dns={}}}",
            BoolOptToStr(data.enabled), StrOptToStr(data.ssid), StrOptToStr(data.passphrase),
            BoolOptToStr(data.dhcp), StrOptToStr(data.address), StrOptToStr(data.gateway),
            StrOptToStr(data.dns));
    }
};

template <>
struct fmt::formatter<sst::config::UplinkData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::UplinkData& data, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "UplinkData{{\n"
                              "  ethernet={},\n"
                              "  wifi={}\n"
                              "}}",
                              data.ethernet, data.wifi);
    }
};
