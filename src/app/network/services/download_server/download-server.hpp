#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/network/models/download-token.hpp"
#include "domain/network/models/recording-summary.hpp"

namespace sst::network {

// Recording catalog + short-lived download tokens (KTD7). Enumerates MP4s from
// the video root, mints per-recording bearer tokens with a TTL, and validates
// them for the HTTP server. No HTTP here — pure logic, fully unit-testable.
class DownloadServer {
   public:
    using Clock = std::function<std::uint64_t()>;  // unix seconds

    DownloadServer(std::filesystem::path video_root, std::filesystem::path thumbnail_root,
                   Clock clock);

    [[nodiscard]] auto Enumerate() const -> std::vector<RecordingSummary>;

    // Mint a token for `recording_id` valid for `ttl_seconds`. nullopt if the
    // recording does not exist on disk.
    auto MintToken(const std::string& recording_id,
                   std::uint64_t ttl_seconds) -> std::optional<DownloadToken>;

    // Mint a token for an explicit file path (the burned overlay L2, #6 F6c),
    // which lives outside the recordings catalog so it never appears in
    // Enumerate(). `id` labels the token (echoed back as the download id).
    // nullopt if the file does not exist.
    auto MintTokenForFile(const std::filesystem::path& file, const std::string& file_id,
                          std::uint64_t ttl_seconds) -> std::optional<DownloadToken>;

    // Resolve a (non-expired) token to the file it authorizes. nullopt if the
    // token is unknown or expired.
    auto ValidateToken(const std::string& token) -> std::optional<std::filesystem::path>;

    // True if any currently-live (unexpired) download token points at `file`.
    // Consulted by proxy retention (U7) so a sweep never deletes a proxy file a
    // client is mid-download on.
    [[nodiscard]] auto IsTokened(const std::filesystem::path& file) const -> bool;

    // Resolve a recording id to its `<id>.jpg` thumbnail under the thumbnail
    // root. nullopt if no such file exists. Unlike video downloads, thumbnails
    // are served untokened (small, non-sensitive preview frames over the P2P
    // link), so the HTTP server resolves by id directly.
    [[nodiscard]] auto ResolveThumbnailPath(const std::string& recording_id) const
        -> std::optional<std::filesystem::path>;

    // Resolve a recording id to its `<id>.mp4` (or raw `.nv12`) under the video
    // root. Public so the overlay-export job (#6 F6c) can locate the clean L1 to
    // burn. nullopt if no such file exists.
    [[nodiscard]] auto ResolveRecordingPath(const std::string& recording_id) const
        -> std::optional<std::filesystem::path>;

   private:
    std::filesystem::path video_root_;
    std::filesystem::path thumbnail_root_;
    Clock clock_;

    mutable std::mutex mtx_;
    struct Entry {
        std::filesystem::path path;
        std::uint64_t expires_at_unix{0};
    };
    std::unordered_map<std::string, Entry> tokens_;  // token -> entry
};

}  // namespace sst::network
