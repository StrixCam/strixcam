#pragma once

#include <cstdint>
#include <string>

namespace sst::storage {

// Identity of one per-camera file of the internal dual-camera proxy — a
// firmware-automatic development artifact (angle/framing/stability/
// color-grading checks and model training), invisible to the app. match_uuid is
// the session's match id (PushSessionConfigCommand.match_uuid), shared by both
// per-camera files of one match; camera_index is the physical sensor index
// (0 = primary, 1 = secondary). The pair is the on-device join key (ssh, by
// match id) — proxy files never cross the app contract.
struct ProxyIdentity {
    std::string match_uuid;
    std::uint32_t camera_index{0};
};

// `a`/`b` are the conventional operand names for a binary `operator==`; this is a
// public free operator whose signature is referenced outside this module, so the
// names stay 1-char.
// NOLINTNEXTLINE(readability-identifier-length)
inline auto operator==(const ProxyIdentity& a, const ProxyIdentity& b) -> bool {
    return a.match_uuid == b.match_uuid && a.camera_index == b.camera_index;
}

}  // namespace sst::storage
