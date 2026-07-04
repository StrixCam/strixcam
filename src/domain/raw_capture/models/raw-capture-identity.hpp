#pragma once

#include <cstdint>
#include <string>

namespace sst::raw_capture {

// Identity of one per-camera raw file in a raw dual-camera capture session.
// capture_group_id is the app-minted id (per the proto contract) shared by both
// per-camera files of one session; camera_index is the physical sensor index
// (0 = primary, 1 = secondary), matching RecordingMetadata.camera_index. The
// pair is the join key the app uses to group the two files.
struct RawCaptureIdentity {
    std::string capture_group_id;
    std::uint32_t camera_index{0};
};

// `a`/`b` are the conventional operand names for a binary `operator==`; this is a
// public free operator whose signature is referenced outside this module, so the
// names stay 1-char.
// NOLINTNEXTLINE(readability-identifier-length)
inline auto operator==(const RawCaptureIdentity& a, const RawCaptureIdentity& b) -> bool {
    return a.capture_group_id == b.capture_group_id && a.camera_index == b.camera_index;
}

}  // namespace sst::raw_capture
