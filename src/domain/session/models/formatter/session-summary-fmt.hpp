#pragma once

#include <fmt/format.h>

#include <string_view>

#include "domain/session/models/session-summary.hpp"

template <>
struct fmt::formatter<sst::session::SessionEndReason> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::session::SessionEndReason reason, FormatContext& ctx) const {
        using sst::session::SessionEndReason;
        std::string_view name = "Unknown";
        switch (reason) {
            case SessionEndReason::kAppStop:
                name = "AppStop";
                break;
            case SessionEndReason::kAutoStop:
                name = "AutoStop";
                break;
            case SessionEndReason::kCameraFailure:
                name = "CameraFailure";
                break;
            case SessionEndReason::kReboot:
                name = "Reboot";
                break;
        }
        return fmt::format_to(ctx.out(), "{}", name);
    }
};

template <>
struct fmt::formatter<sst::session::LastSessionSummary> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::session::LastSessionSummary& summary, FormatContext& ctx) const {
        return fmt::format_to(
            ctx.out(), "LastSessionSummary{{match={}, reason={}, end_clock={}s, file_valid={}}}",
            summary.match_uuid, summary.end_reason, summary.end_clock_seconds, summary.file_valid);
    }
};
