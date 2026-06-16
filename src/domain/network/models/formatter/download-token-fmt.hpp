#pragma once

#include <fmt/format.h>

#include "domain/network/models/download-token.hpp"

template <>
struct fmt::formatter<sst::network::DownloadToken> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::network::DownloadToken& token, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "DownloadToken{{recording={}, expires_at={}}}",
                              token.recording_id, token.expires_at_unix);
    }
};
