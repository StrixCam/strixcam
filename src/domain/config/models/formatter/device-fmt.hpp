#pragma once

#include <fmt/format.h>

#include "domain/config/models/device.hpp"
#include "fmt-helper.hpp"

template <>
struct fmt::formatter<sst::config::DeviceStaticIpData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceStaticIpData& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceStaticIpData{{\n"
                              "  enabled={},\n"
                              "  ip_address={},\n"
                              "  subnet_mask={},\n"
                              "  gateway={}\n"
                              "}}",
                              BoolOptToStr(data.enabled), StrOptToStr(data.ip_address),
                              StrOptToStr(data.subnet_mask), StrOptToStr(data.gateway));
    }
};

template <>
struct fmt::formatter<sst::config::DeviceConnectivityWifiDirectData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceConnectivityWifiDirectData& data,
                FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceConnectivityWifiDirectData{{\n"
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

template <>
struct fmt::formatter<sst::config::DeviceConnectivityEthernetData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceConnectivityEthernetData& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::ObjOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceConnectivityEthernetData{{\n"
                              "  enabled={},\n"
                              "  static_ip={}\n"
                              "}}",
                              BoolOptToStr(data.enabled), ObjOptToStr(data.static_ip));
    }
};

template <>
struct fmt::formatter<sst::config::DeviceConnectivityBluetoothData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceConnectivityBluetoothData& data,
                FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::StrOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceConnectivityBluetoothData{{\n"
                              "  enabled={},\n"
                              "  name={},\n"
                              "  password={}\n"
                              "}}",
                              BoolOptToStr(data.enabled), StrOptToStr(data.name),
                              StrOptToStr(data.password));
    }
};

template <>
struct fmt::formatter<sst::config::DeviceConnectivityData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceConnectivityData& data, FormatContext& ctx) const {
        using sst::config::ObjOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceConnectivityData{{\n"
                              "  wifi_direct={},\n"
                              "  ethernet={},\n"
                              "  bluetooth={}\n"
                              "}}",
                              ObjOptToStr(data.wifi_direct), ObjOptToStr(data.ethernet),
                              ObjOptToStr(data.bluetooth));
    }
};

template <>
struct fmt::formatter<sst::config::DeviceData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::DeviceData& data, FormatContext& ctx) const {
        using sst::config::ObjOptToStr;
        using sst::config::StrOptToStr;

        return fmt::format_to(ctx.out(),
                              "DeviceData{{\n"
                              "  name={},\n"
                              "  model={},\n"
                              "  version={},\n"
                              "  serial_number={},\n"
                              "  manufacturer={},\n"
                              "  timezone={},\n"
                              "  timestamp={},\n"
                              "  connectivity={}\n"
                              "}}",
                              StrOptToStr(data.name), StrOptToStr(data.model),
                              StrOptToStr(data.version), StrOptToStr(data.serial_number),
                              StrOptToStr(data.manufacturer), StrOptToStr(data.timezone),
                              StrOptToStr(data.timestamp), ObjOptToStr(data.connectivity));
    }
};
