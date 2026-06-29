#pragma once

#include <fmt/format.h>

#include "domain/overlay/models/formatter/render-scene-fmt.hpp"  // IWYU pragma: keep
#include "domain/overlay/models/overlay-timeline.hpp"

template <>
struct fmt::formatter<sst::overlay::TimelineEvent> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::TimelineEvent& event, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "TimelineEvent{{at_ms={}, scene={}}}", event.at_ms,
                              event.scene);
    }
};

template <>
struct fmt::formatter<sst::overlay::OverlayTimeline> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const sst::overlay::OverlayTimeline& timeline, Ctx& ctx) const {
        return fmt::format_to(ctx.out(), "OverlayTimeline{{anchor_ms={}, events={}}}",
                              timeline.anchor_ms, timeline.events.size());
    }
};
