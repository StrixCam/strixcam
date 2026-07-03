#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace sst::adapters::raw_capture {

// Bounds total training-proxy storage. The proxy writes small per-camera H.264
// files (raw__<group>__cam<N>.mp4) flat at the video root, one pair per match —
// they accumulate. Sweep() deletes the OLDEST complete groups once the total
// exceeds `max_total_bytes`, so training footage can't grow unbounded, while
// never touching a group that is still being written (recent mtime) or is being
// downloaded (the caller's protection predicate).
//
// Deliberately delete-oldest-first: the just-finished match (newest) is what the
// app is about to download, so it survives; storage pressure evicts the tail.
class ProxyRetention {
   public:
    ProxyRetention(std::filesystem::path video_dir, std::uint64_t max_total_bytes);

    // Delete oldest proxy groups until the total is within budget. A group is
    // KEPT when either: `is_path_protected(file)` returns true for any of its
    // files (e.g. a live download token), or its newest file was modified within
    // the write-grace window (still being captured). Returns bytes freed. A
    // max_total_bytes of 0 disables retention (no-op).
    auto Sweep(const std::function<bool(const std::filesystem::path&)>& is_path_protected)
        -> std::uint64_t;

   private:
    const std::filesystem::path video_dir_;
    const std::uint64_t max_total_bytes_;
};

}  // namespace sst::adapters::raw_capture
