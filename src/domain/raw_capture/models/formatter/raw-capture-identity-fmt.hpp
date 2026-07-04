#pragma once

#include <fmt/format.h>

#include "domain/raw_capture/models/raw-capture-identity.hpp"

template <>
struct fmt::formatter<sst::raw_capture::RawCaptureIdentity> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::raw_capture::RawCaptureIdentity& identity, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "RawCaptureIdentity{{group={}, camera_index={}}}",
                              identity.capture_group_id, identity.camera_index);
    }
};
