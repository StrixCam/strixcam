#pragma once

#include <fmt/format.h>

#include <string_view>

#include "domain/overlay/models/overlay-enums.hpp"

namespace sst::overlay::detail {

inline auto ToString(OverlayShape s) -> std::string_view {
    switch (s) {
        case OverlayShape::kRect:
            return "Rect";
        case OverlayShape::kText:
            return "Text";
        case OverlayShape::kCircle:
            return "Circle";
        case OverlayShape::kUnknown:
            break;
    }
    return "Unknown";
}

inline auto ToString(OverlayBinding b) -> std::string_view {
    switch (b) {
        case OverlayBinding::kStatic:
            return "Static";
        case OverlayBinding::kScoreA:
            return "ScoreA";
        case OverlayBinding::kScoreB:
            return "ScoreB";
        case OverlayBinding::kScoreVs:
            return "ScoreVs";
        case OverlayBinding::kTeamAName:
            return "TeamAName";
        case OverlayBinding::kTeamBName:
            return "TeamBName";
        case OverlayBinding::kMatchClock:
            return "MatchClock";
        case OverlayBinding::kPeriodLabel:
            return "PeriodLabel";
    }
    return "Static";
}

inline auto ToString(TextAlign a) -> std::string_view {
    switch (a) {
        case TextAlign::kLeft:
            return "Left";
        case TextAlign::kCenter:
            return "Center";
        case TextAlign::kRight:
            return "Right";
    }
    return "Left";
}

inline auto ToString(FontWeight w) -> std::string_view {
    return w == FontWeight::kBold ? "Bold" : "Normal";
}

inline auto ToString(PeriodLabelState s) -> std::string_view {
    switch (s) {
        case PeriodLabelState::kInPlay:
            return "InPlay";
        case PeriodLabelState::kHalfTime:
            return "HalfTime";
        case PeriodLabelState::kFullTime:
            return "FullTime";
    }
    return "InPlay";
}

}  // namespace sst::overlay::detail

#define SST_OVERLAY_ENUM_FORMATTER(Type)                                                    \
    template <>                                                                             \
    struct fmt::formatter<sst::overlay::Type> {                                             \
        static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); } \
        template <typename Ctx>                                                             \
        auto format(sst::overlay::Type v, Ctx& ctx) const {                                 \
            return fmt::format_to(ctx.out(), "{}", sst::overlay::detail::ToString(v));      \
        }                                                                                   \
    }

SST_OVERLAY_ENUM_FORMATTER(OverlayShape);
SST_OVERLAY_ENUM_FORMATTER(OverlayBinding);
SST_OVERLAY_ENUM_FORMATTER(TextAlign);
SST_OVERLAY_ENUM_FORMATTER(FontWeight);
SST_OVERLAY_ENUM_FORMATTER(PeriodLabelState);

#undef SST_OVERLAY_ENUM_FORMATTER
