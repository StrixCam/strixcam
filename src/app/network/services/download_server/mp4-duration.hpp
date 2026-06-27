#pragma once

#include <cstdint>
#include <filesystem>

namespace sst::network {

// Best-effort parse of an mp4's movie duration (whole seconds) from its
// moov/mvhd box — the value mp4mux writes on finalize. Returns 0 when the file
// can't be opened or no mvhd is found; callers treat 0 as "unknown duration".
//
// Reads box headers by seeking (never loads the whole file), so it is cheap even
// for multi-gigabyte recordings.
auto ProbeMp4DurationSeconds(const std::filesystem::path& path) -> std::uint64_t;

}  // namespace sst::network
