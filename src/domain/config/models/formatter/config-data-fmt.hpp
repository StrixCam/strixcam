#pragma once

#include <fmt/format.h>

#include "calibration-fmt.hpp"  // IWYU pragma: keep
#include "device-fmt.hpp"       // IWYU pragma: keep
#include "domain/config/models/config-data.hpp"
#include "storage-fmt.hpp"        // IWYU pragma: keep
#include "uplink-config-fmt.hpp"  // IWYU pragma: keep
#include "wifi-direct-fmt.hpp"    // IWYU pragma: keep

template <>
struct fmt::formatter<sst::config::ConfigData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::ConfigData& data, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "ConfigData{{\n"
                              "  calibration={},\n"
                              "  device={},\n"
                              "  storage={},\n"
                              "  wifi_direct={},\n"
                              "  uplink={}\n"
                              "}}",
                              data.calibration, data.device, data.storage, data.wifi_direct,
                              data.uplink);
    }
};
