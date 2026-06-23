#include "app/network/services/download_server/download-server.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <system_error>
#include <utility>

#include "domain/common/utils/uuid.hpp"
#include "domain/storage/services/raw-capture-naming.hpp"

namespace sst::network {

namespace fs = std::filesystem;

namespace {
constexpr const char* kMp4Extension = ".mp4";
constexpr const char* kRawExtension = ".nv12";

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

DownloadServer::DownloadServer(fs::path video_root, Clock clock)
    : video_root_(std::move(video_root)), clock_(std::move(clock)) {}

auto DownloadServer::Enumerate() const -> std::vector<RecordingSummary> {
    std::vector<RecordingSummary> out;
    std::error_code err;
    if (!fs::exists(video_root_, err)) {
        return out;
    }
    for (auto entry = fs::recursive_directory_iterator(video_root_, err);
         !err && entry != fs::recursive_directory_iterator(); entry.increment(err)) {
        const fs::path& path = entry->path();
        if (!entry->is_regular_file(err)) {
            continue;
        }
        const auto ext = path.extension();
        RecordingSummary summary;
        if (ext == kMp4Extension) {
            // Final-match recording: is_raw stays false, raw identity unset.
            summary.recording_id = path.stem().string();
            summary.thumbnail_id = summary.recording_id;
        } else if (ext == kRawExtension) {
            // Raw dual-camera file: parse identity from the filename so the app
            // can group the cam-0/cam-1 pair by capture_group_id.
            const auto identity =
                sst::storage::raw_capture_naming::ParseFileName(path.filename().string());
            if (!identity) {
                continue;  // a .nv12 that isn't a raw-capture file we wrote
            }
            summary.recording_id = path.stem().string();
            summary.is_raw = true;
            summary.camera_index = identity->camera_index;
            summary.capture_group_id = identity->capture_group_id;
        } else {
            continue;
        }
        std::error_code size_ec;
        summary.size_bytes = static_cast<std::uint64_t>(fs::file_size(path, size_ec));
        summary.started_at_unix = LastWriteUnix(path);
        out.push_back(std::move(summary));
    }
    return out;
}

auto DownloadServer::ResolveRecordingPath(const std::string& recording_id) const
    -> std::optional<fs::path> {
    std::error_code err;
    if (recording_id.empty() || !fs::exists(video_root_, err)) {
        return std::nullopt;
    }
    for (auto entry = fs::recursive_directory_iterator(video_root_, err);
         !err && entry != fs::recursive_directory_iterator(); entry.increment(err)) {
        const fs::path& path = entry->path();
        const auto ext = path.extension();
        if (entry->is_regular_file(err) && (ext == kMp4Extension || ext == kRawExtension) &&
            path.stem().string() == recording_id) {
            return path;
        }
    }
    return std::nullopt;
}

auto DownloadServer::MintToken(const std::string& recording_id,
                               std::uint64_t ttl_seconds) -> std::optional<DownloadToken> {
    auto path = ResolveRecordingPath(recording_id);
    if (!path) {
        spdlog::warn("DownloadServer::MintToken: recording {} not found", recording_id);
        return std::nullopt;
    }
    DownloadToken token;
    token.recording_id = recording_id;
    token.token = sst::common::utils::MakeSecureToken();
    token.expires_at_unix = clock_() + ttl_seconds;
    {
        std::lock_guard lock(mtx_);
        tokens_[token.token] = Entry{.path = *path, .expires_at_unix = token.expires_at_unix};
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

}  // namespace sst::network
