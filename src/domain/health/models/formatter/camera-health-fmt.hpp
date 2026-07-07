#pragma once

#include <fmt/format.h>

#include <string_view>

#include "domain/health/models/camera-health.hpp"

template <>
struct fmt::formatter<sst::health::CameraHealth> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(sst::health::CameraHealth health, FormatContext& ctx) const {
        using sst::health::CameraHealth;
        std::string_view name = "Unknown";
        switch (health) {
            case CameraHealth::kOk:
                name = "Ok";
                break;
            case CameraHealth::kRecovering:
                name = "Recovering";
                break;
            case CameraHealth::kDown:
                name = "Down";
                break;
        }
        return fmt::format_to(ctx.out(), "{}", name);
    }
};
