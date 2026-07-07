#pragma once

#include <fmt/format.h>

#include "domain/storage/models/proxy-identity.hpp"

template <>
struct fmt::formatter<sst::storage::ProxyIdentity> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::storage::ProxyIdentity& identity, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "ProxyIdentity{{match={}, camera_index={}}}",
                              identity.match_uuid, identity.camera_index);
    }
};
