#pragma once

#include <cstdint>
#include <string>

namespace sst::network {

// Disk-enumerated recording metadata for ListRecordings. The recording_id is
// the file stem; sport/teams are filled from the in-memory session when
// available. Internal proxy files (proxy__<match>__cam<N>.mp4) are excluded
// from enumeration entirely — they never surface on the wire (the on-demand
// overlay export <match>-overlay.mp4 DOES enumerate, like any recording).
struct RecordingSummary {
    std::string recording_id;
    std::uint64_t size_bytes{0};
    std::uint64_t started_at_unix{0};
    std::uint64_t duration_s{0};
    std::string thumbnail_id;
    std::string sport;
    std::string teams;
};

}  // namespace sst::network
