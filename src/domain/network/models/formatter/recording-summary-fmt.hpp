#pragma once

#include <fmt/format.h>

#include "domain/network/models/recording-summary.hpp"

template <>
struct fmt::formatter<sst::network::RecordingSummary> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::network::RecordingSummary& summary, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "RecordingSummary{{id={}, size={} bytes, started_at={}, "
                              "duration={}s, thumbnail={}, is_raw={}, camera_index={}, group={}}}",
                              summary.recording_id, summary.size_bytes, summary.started_at_unix,
                              summary.duration_s, summary.thumbnail_id, summary.is_raw,
                              summary.camera_index, summary.capture_group_id);
    }
};
