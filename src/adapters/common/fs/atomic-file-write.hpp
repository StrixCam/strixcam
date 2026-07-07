#pragma once

#include <sys/types.h>

#include <string>
#include <string_view>

// Crash-safe file persistence shared by the JSON stores (last-session summary,
// uplink config). An in-place `ofstream(path, trunc)` write is a corruption
// window: a crash mid-write leaves a truncated/partial file where the previous
// good record used to be.
namespace sst::adapters::fs_common {

// Write `contents` to `<path>.tmp` (created with `mode`, umask-masked, so the
// bytes are NEVER visible under a wider mode — no chmod-after race), fsync it,
// then atomically rename() it over `path`. Readers observe either the previous
// complete file or the new complete file, never a partial write. Returns false
// (and removes the temp file) on any failure, leaving `path` untouched.
auto WriteFileAtomic(const std::string& path, std::string_view contents, mode_t mode) -> bool;

}  // namespace sst::adapters::fs_common
