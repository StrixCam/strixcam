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

    DownloadServer(std::filesystem::path video_root, Clock clock);

    [[nodiscard]] auto Enumerate() const -> std::vector<RecordingSummary>;

    // Mint a token for `recording_id` valid for `ttl_seconds`. nullopt if the
    // recording does not exist on disk.
    auto MintToken(const std::string& recording_id, std::uint64_t ttl_seconds)
        -> std::optional<DownloadToken>;

    // Resolve a (non-expired) token to the file it authorizes. nullopt if the
    // token is unknown or expired.
    auto ValidateToken(const std::string& token) -> std::optional<std::filesystem::path>;

   private:
    [[nodiscard]] auto ResolveRecordingPath(const std::string& recording_id) const
        -> std::optional<std::filesystem::path>;

    std::filesystem::path video_root_;
    Clock clock_;

    mutable std::mutex mtx_;
    struct Entry {
        std::filesystem::path path;
        std::uint64_t expires_at_unix{0};
    };
    std::unordered_map<std::string, Entry> tokens_;  // token -> entry
};

}  // namespace sst::network
