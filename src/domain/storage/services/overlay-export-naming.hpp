#pragma once

#include <string>
#include <string_view>

// On-disk naming convention for the on-demand overlay export (L2), shared by
// the burner job (writer) and its tests. The L2 PERSISTS in the same per-match
// directory as its clean L1 — no separate exports dir, no post-download
// deletion — and its stem doubles as its recording id: it enumerates and
// downloads like any recording, clearly marked as the overlay variant.
//
// Layout:  <l1_stem>-overlay.mp4   (beside <l1_stem>.mp4)
namespace sst::storage::overlay_export_naming {

inline constexpr std::string_view kOverlayMarker = "-overlay";
inline constexpr std::string_view kExtension = ".mp4";

// The L2 filename for a clean recording whose file stem is `l1_stem`
// (the stem is the match id — see RecordingService's ComposeFile).
inline auto FileName(std::string_view l1_stem) -> std::string {
    return std::string(l1_stem) + std::string(kOverlayMarker) + std::string(kExtension);
}

}  // namespace sst::storage::overlay_export_naming
