#pragma once

#include <cstdint>
#include <string>

namespace sst::session {

// In-memory snapshot of PushSessionConfigCommand. Held only for the duration of
// a session (R11/R12) — never persisted across sessions. The app is the source
// of truth; this is the camera's working copy for the active match.
struct SessionConfig {
    std::string match_uuid;
    std::string user_uuid;
    std::string sport;
    std::int32_t num_periods{0};
    std::int32_t period_length_seconds{0};
    std::string rtmp_url;    // empty when the proto optional is unset
    std::string stream_key;  // empty when the proto optional is unset
    std::string video_output_path;
    std::string thumbnail_output_path;

    std::string team_a_id;
    std::string team_b_id;
    std::string team_a_name;
    std::string team_b_name;
    std::string team_a_color_hex;
    std::string team_b_color_hex;

    // Auto-stop safety net: minutes of app absence before the firmware ends the
    // session on its own (finalize + summary + group teardown). 0 = the app did
    // not set it → the firmware default (30 min) applies. Wire field lands with
    // the U2 contract bump; until then this stays at the default.
    std::int32_t auto_stop_minutes{0};
};

}  // namespace sst::session
