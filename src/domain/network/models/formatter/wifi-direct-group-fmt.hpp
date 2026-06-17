#pragma once

#include <fmt/format.h>

#include "domain/network/models/wifi-direct-group.hpp"

template <>
struct fmt::formatter<sst::network::WifiDirectGroup> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::network::WifiDirectGroup& group, FormatContext& ctx) const {
        // Passphrase intentionally not logged in full.
        return fmt::format_to(ctx.out(),
                              "WifiDirectGroup{{ssid={}, psk=<{} chars>, iface={}, go_ip={}, "
                              "role={}}}",
                              group.ssid, group.psk.size(), group.group_interface,
                              group.group_owner_ip, group.role);
    }
};
