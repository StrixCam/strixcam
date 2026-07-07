#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace sst::adapters::storage {

// Bounds total internal-proxy storage. The proxy writes small per-camera H.264
// files (proxy__<match_uuid>__cam<N>.mp4) into each per-match directory under
// the video root (the sweep walks recursively), one pair per match — they
// accumulate. Sweep() deletes the OLDEST complete matches' pairs once the total
// exceeds `max_total_bytes`, so development footage can't grow unbounded, while
// never touching a pair that is still being written (recent mtime) or that the
// caller protects (the predicate).
//
// Deliberately delete-oldest-first: the just-finished match (newest) is what a
// developer is most likely to pull off the device (ssh, by match id), so it
// survives; storage pressure evicts the tail.
class ProxyRetention {
   public:
    ProxyRetention(std::filesystem::path video_dir, std::uint64_t max_total_bytes);

    // Delete oldest proxy pairs until the total is within budget. A pair is
    // KEPT when either: `is_path_protected(file)` returns true for any of its
    // files (a caller-side hold), or its newest file was modified within the
    // write-grace window (still being captured). Returns bytes freed. A
    // max_total_bytes of 0 disables retention (no-op).
    auto Sweep(const std::function<bool(const std::filesystem::path&)>& is_path_protected)
        -> std::uint64_t;

   private:
    const std::filesystem::path video_dir_;
    const std::uint64_t max_total_bytes_;
};

}  // namespace sst::adapters::storage
