#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "domain/storage/models/proxy-identity.hpp"

// On-disk naming convention for the internal dual-camera proxy files, shared by
// the writer (proxy sink) and the enumerator (download server) so the two never
// drift on the format. The proxy is a firmware-internal development artifact:
// the enumerator uses ParseFileName as the EXCLUSION discriminator (proxy files
// never appear in ListRecordings; retrieval is on-device, by match id).
//
// Layout:  proxy__<match_uuid>__cam<N>.mp4
//
// The `proxy__` prefix marks a per-camera proxy file (final-match recordings
// are `.mp4` too and live in the same per-match dir — the prefix, not the
// extension or location, is the discriminator). The middle segment is the
// session's match_uuid (PushSessionConfigCommand), and the `__cam<N>` suffix is
// the physical sensor index. Each file is a small H.264 MP4 (854x480@15, see
// proxy-launch); one per camera. Double-underscore delimiters keep parsing
// unambiguous as long as the match id contains no `__` (the app mints UUIDs,
// which do not).
namespace sst::storage::proxy_naming {

inline constexpr std::string_view kPrefix = "proxy__";
inline constexpr std::string_view kCameraMarker = "__cam";
inline constexpr std::string_view kExtension = ".mp4";

// `id` names a proxy identity; this is a public API referenced outside this
// module (writer + download server), so the parameter name is part of the
// contract and stays as-is.
// NOLINTNEXTLINE(readability-identifier-length)
inline auto FileName(const ProxyIdentity& id) -> std::string {
    return std::string(kPrefix) + id.match_uuid + std::string(kCameraMarker) +
           std::to_string(id.camera_index) + std::string(kExtension);
}

// Parse a proxy filename back into its identity. Returns std::nullopt for any
// name that is not a proxy file (e.g. an `.mp4` final recording), so the
// enumerator can use it as the exclusion filter.
inline auto ParseFileName(std::string_view file_name) -> std::optional<ProxyIdentity> {
    if (file_name.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    if (file_name.size() < kExtension.size() ||
        file_name.substr(file_name.size() - kExtension.size()) != kExtension) {
        return std::nullopt;
    }
    const auto marker_pos = file_name.rfind(kCameraMarker);
    if (marker_pos == std::string_view::npos || marker_pos <= kPrefix.size()) {
        return std::nullopt;
    }

    const auto match_start = kPrefix.size();
    const auto match = file_name.substr(match_start, marker_pos - match_start);
    const auto index_start = marker_pos + kCameraMarker.size();
    const auto index_len = file_name.size() - kExtension.size() - index_start;
    const auto index_str = file_name.substr(index_start, index_len);
    if (match.empty() || index_str.empty()) {
        return std::nullopt;
    }

    constexpr std::uint32_t kDecimalBase = 10;
    std::uint32_t camera_index = 0;
    for (const char digit : index_str) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        camera_index = camera_index * kDecimalBase + static_cast<std::uint32_t>(digit - '0');
    }

    return ProxyIdentity{.match_uuid = std::string(match), .camera_index = camera_index};
}

}  // namespace sst::storage::proxy_naming
