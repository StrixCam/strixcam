#include "app/network/services/download_server/download-server.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

#include "app/network/services/download_server/mp4-duration.hpp"
#include "domain/common/utils/uuid.hpp"
#include "domain/raw_capture/services/raw-capture-naming.hpp"

namespace sst::network {

namespace fs = std::filesystem;

namespace {
constexpr const char* kMp4Extension = ".mp4";
constexpr const char* kThumbnailExtension = ".jpg";

auto LastWriteUnix(const fs::path& path) -> std::uint64_t {
    std::error_code err;
    const auto ftime = fs::last_write_time(path, err);
    if (err) {
        return 0;
    }
    // Approximate: map file_clock to system_clock seconds since epoch.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}
}  // namespace

DownloadServer::DownloadServer(fs::path video_root, fs::path thumbnail_root, Clock clock)
    : video_root_(std::move(video_root)),
      thumbnail_root_(std::move(thumbnail_root)),
      clock_(std::move(clock)) {}

auto DownloadServer::Enumerate() const -> std::vector<RecordingSummary> {
    std::vector<RecordingSummary> out;
    std::error_code err;
    if (!fs::exists(video_root_, err)) {
        return out;
    }
    for (auto entry = fs::recursive_directory_iterator(video_root_, err);
         !err && entry != fs::recursive_directory_iterator(); entry.increment(err)) {
        const fs::path& path = entry->path();
        // Per-entry error code, NOT the iterator's `err`: reusing `err` makes one
        // failed stat (a file deleted mid-scan — the proxy-retention sweep deletes
        // concurrently) silently END the whole walk via the `!err` loop condition,
        // truncating the recordings list.
        std::error_code stat_ec;
        if (!entry->is_regular_file(stat_ec) || stat_ec) {
            continue;
        }
        const auto ext = path.extension();
        if (ext != kMp4Extension) {
            continue;
        }
        RecordingSummary summary;
        // Both the training proxy and the final recording are .mp4 now; the
        // `raw__<group>__cam<N>` filename prefix (parsed here) is the sole
        // discriminator — proxies carry it, final recordings do not (both live
        // inside the per-match subdirs; the walk above is recursive).
        const auto identity =
            sst::raw_capture::raw_capture_naming::ParseFileName(path.filename().string());
        if (identity) {
            // Raw dual-camera training proxy: parse identity so the app can group
            // the cam-0/cam-1 pair by capture_group_id.
            summary.recording_id = path.stem().string();
            summary.is_raw = true;
            summary.camera_index = identity->camera_index;
            summary.capture_group_id = identity->capture_group_id;
        } else {
            // Final-match recording: is_raw stays false, raw identity unset.
            summary.recording_id = path.stem().string();
            summary.thumbnail_id = summary.recording_id;
            summary.duration_s = ProbeMp4DurationSeconds(path);
        }
        std::error_code size_ec;
        summary.size_bytes = static_cast<std::uint64_t>(fs::file_size(path, size_ec));
        summary.started_at_unix = LastWriteUnix(path);
        out.push_back(std::move(summary));
    }
    return out;
}

namespace {

// One recursive stem-match scan for both Resolve* lookups (they differed only in
// root + extension). Per-entry error codes so one failed stat skips that entry
// instead of ending the walk (same rationale as Enumerate above).
auto FindByStem(const fs::path& root, const std::string& stem,
                const char* extension) -> std::optional<fs::path> {
    std::error_code err;
    if (stem.empty() || !fs::exists(root, err)) {
        return std::nullopt;
    }
    for (auto entry = fs::recursive_directory_iterator(root, err);
         !err && entry != fs::recursive_directory_iterator(); entry.increment(err)) {
        const fs::path& path = entry->path();
        std::error_code stat_ec;
        if (entry->is_regular_file(stat_ec) && !stat_ec && path.extension() == extension &&
            path.stem().string() == stem) {
            return path;
        }
    }
    return std::nullopt;
}

}  // namespace

auto DownloadServer::ResolveRecordingPath(const std::string& recording_id) const
    -> std::optional<fs::path> {
    return FindByStem(video_root_, recording_id, kMp4Extension);
}

auto DownloadServer::ResolveThumbnailPath(const std::string& recording_id) const
    -> std::optional<fs::path> {
    return FindByStem(thumbnail_root_, recording_id, kThumbnailExtension);
}

auto DownloadServer::MintToken(const std::string& recording_id,
                               std::uint64_t ttl_seconds) -> std::optional<DownloadToken> {
    auto path = ResolveRecordingPath(recording_id);
    if (!path) {
        spdlog::warn("DownloadServer::MintToken: recording {} not found", recording_id);
        return std::nullopt;
    }
    return MintFor(*path, recording_id, ttl_seconds);
}

auto DownloadServer::MintTokenForFile(const fs::path& file, const std::string& file_id,
                                      std::uint64_t ttl_seconds) -> std::optional<DownloadToken> {
    std::error_code err;
    if (!fs::exists(file, err) || !fs::is_regular_file(file, err)) {
        spdlog::warn("DownloadServer::MintTokenForFile: {} missing", file.string());
        return std::nullopt;
    }
    return MintFor(file, file_id, ttl_seconds);
}

// Shared token construction for both Mint entry points.
auto DownloadServer::MintFor(const fs::path& file, const std::string& file_id,
                             std::uint64_t ttl_seconds) -> DownloadToken {
    DownloadToken token;
    token.recording_id = file_id;
    token.token = sst::common::utils::MakeSecureToken();
    token.expires_at_unix = clock_() + ttl_seconds;
    {
        std::lock_guard lock(mtx_);
        tokens_[token.token] = Entry{.path = file, .expires_at_unix = token.expires_at_unix};
    }
    return token;
}

auto DownloadServer::ValidateToken(const std::string& token) -> std::optional<fs::path> {
    std::lock_guard lock(mtx_);
    const auto entry = tokens_.find(token);
    if (entry == tokens_.end()) {
        return std::nullopt;
    }
    if (clock_() >= entry->second.expires_at_unix) {
        tokens_.erase(entry);
        return std::nullopt;
    }
    return entry->second.path;
}

auto DownloadServer::IsTokened(const fs::path& file) const -> bool {
    const auto now = clock_();
    std::lock_guard lock(mtx_);
    return std::ranges::any_of(tokens_, [&](const auto& token) {
        return token.second.expires_at_unix > now && token.second.path == file;
    });
}

}  // namespace sst::network
