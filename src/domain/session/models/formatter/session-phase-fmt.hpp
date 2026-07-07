#pragma once

#include <fmt/format.h>

#include <string_view>

#include "domain/session/models/session-phase.hpp"

template <>
struct fmt::formatter<sst::session::SessionPhase> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::session::SessionPhase phase, FormatContext& ctx) const {
        using sst::session::SessionPhase;
        std::string_view name = "Unknown";
        switch (phase) {
            case SessionPhase::kIdle:
                name = "Idle";
                break;
            case SessionPhase::kConfigured:
                name = "Configured";
                break;
            case SessionPhase::kReady:
                name = "Ready";
                break;
            case SessionPhase::kRecording:
                name = "Recording";
                break;
            case SessionPhase::kFinalizing:
                name = "Finalizing";
                break;
        }
        return fmt::format_to(ctx.out(), "{}", name);
    }
};
