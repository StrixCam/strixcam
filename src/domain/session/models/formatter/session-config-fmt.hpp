#pragma once

#include <fmt/format.h>

#include "domain/session/models/session-config.hpp"

template <>
struct fmt::formatter<sst::session::SessionConfig> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::session::SessionConfig& config, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(),
                              "SessionConfig{{match={}, user={}, sport={}, periods={}x{}s, "
                              "teams=[{}|{} vs {}|{}], video={}, thumb={}, auto_stop={}min}}",
                              config.match_uuid, config.user_uuid, config.sport, config.num_periods,
                              config.period_length_seconds, config.team_a_name, config.team_a_id,
                              config.team_b_name, config.team_b_id, config.video_output_path,
                              config.thumbnail_output_path, config.auto_stop_minutes);
    }
};
