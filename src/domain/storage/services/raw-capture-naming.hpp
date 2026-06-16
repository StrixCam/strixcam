#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "domain/storage/models/raw-capture-identity.hpp"

// On-disk naming convention for raw dual-camera capture files, shared by the
// writer (raw-capture sink) and the enumerator (download server) so the two
// never drift on the format.
//
// Layout:  raw__<capture_group_id>__cam<N>.nv12
//
// The `raw__` prefix marks a raw file (final-match recordings are `.mp4`), the
// middle segment is the app-minted capture_group_id, and the `__cam<N>` suffix
// is the physical sensor index. Frames are concatenated NV12 planes (the demo's
// fixed capture geometry); no per-frame header. Double-underscore delimiters
// keep parsing unambiguous as long as the group id contains no `__` (app mints
// UUIDs, which do not).
namespace sst::storage::raw_capture_naming {

inline constexpr std::string_view kPrefix = "raw__";
inline constexpr std::string_view kCameraMarker = "__cam";
inline constexpr std::string_view kExtension = ".nv12";

inline auto FileName(const RawCaptureIdentity& id) -> std::string {
    return std::string(kPrefix) + id.capture_group_id + std::string(kCameraMarker) +
           std::to_string(id.camera_index) + std::string(kExtension);
}

// Parse a raw-capture filename back into its identity. Returns std::nullopt for
// any name that is not a raw-capture file (e.g. an `.mp4` final recording), so
// the enumerator can use it as a filter.
inline auto ParseFileName(std::string_view file_name) -> std::optional<RawCaptureIdentity> {
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

    const auto group_start = kPrefix.size();
    const auto group = file_name.substr(group_start, marker_pos - group_start);
    const auto index_start = marker_pos + kCameraMarker.size();
    const auto index_len = file_name.size() - kExtension.size() - index_start;
    const auto index_str = file_name.substr(index_start, index_len);
    if (group.empty() || index_str.empty()) {
        return std::nullopt;
    }

    std::uint32_t camera_index = 0;
    for (const char digit : index_str) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        camera_index = camera_index * 10 + static_cast<std::uint32_t>(digit - '0');
    }

    return RawCaptureIdentity{.capture_group_id = std::string(group), .camera_index = camera_index};
}

}  // namespace sst::storage::raw_capture_naming
