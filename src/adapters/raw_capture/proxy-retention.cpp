#include "adapters/raw_capture/proxy-retention.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "domain/raw_capture/services/raw-capture-naming.hpp"

namespace sst::adapters::raw_capture {

namespace fs = std::filesystem;

namespace {
// A group whose newest file changed within this window is treated as still being
// captured and is never swept, even if it is the oldest — the active proxy is
// writing into it.
constexpr auto kWriteGrace = std::chrono::seconds(20);

struct Group {
    std::uint64_t bytes{0};
    // Seed to min() so std::max() below always takes the first real file mtime —
    // a default-constructed file_time_type is the file-clock epoch, which can sit
    // AFTER a valid past mtime and wrongly make the group look freshly written.
    fs::file_time_type newest{fs::file_time_type::min()};
    std::vector<fs::path> files;
};
}  // namespace

ProxyRetention::ProxyRetention(std::filesystem::path video_dir, std::uint64_t max_total_bytes)
    : video_dir_(std::move(video_dir)), max_total_bytes_(max_total_bytes) {}

auto ProxyRetention::Sweep(
    const std::function<bool(const std::filesystem::path&)>& is_path_protected) -> std::uint64_t {
    if (max_total_bytes_ == 0) {
        return 0;  // retention disabled
    }
    std::error_code err;
    if (!fs::exists(video_dir_, err)) {
        return 0;
    }

    // Group the raw__<group>__cam<N>.mp4 files by capture_group_id. Recursive:
    // proxies live inside each per-match dir (beside the final MP4), not flat at
    // the root, so the sweep must descend to find them.
    std::map<std::string, Group> groups;
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(video_dir_, err), end; !err && it != end;
         it.increment(err)) {
        if (!it->is_regular_file(err)) {
            continue;
        }
        const auto identity =
            sst::raw_capture::raw_capture_naming::ParseFileName(it->path().filename().string());
        if (!identity) {
            continue;  // final recordings / other files are not proxy footage
        }
        const auto size = static_cast<std::uint64_t>(fs::file_size(it->path(), err));
        const auto mtime = fs::last_write_time(it->path(), err);
        auto& group = groups[identity->capture_group_id];
        group.bytes += size;
        group.files.push_back(it->path());
        group.newest = std::max(group.newest, mtime);
        total += size;
    }

    if (total <= max_total_bytes_) {
        return 0;
    }

    // Oldest first, by the group's most-recently-written file.
    std::vector<std::pair<std::string, Group*>> ordered;
    ordered.reserve(groups.size());
    for (auto& [gid, group] : groups) {
        ordered.emplace_back(gid, &group);
    }
    std::ranges::sort(ordered, [](const auto& lhs, const auto& rhs) {
        return lhs.second->newest < rhs.second->newest;
    });

    const auto now = fs::file_time_type::clock::now();
    std::uint64_t freed = 0;
    for (const auto& [gid, group] : ordered) {
        if (total <= max_total_bytes_) {
            break;
        }
        if (now - group->newest < kWriteGrace) {
            continue;  // still being captured — leave it
        }
        if (std::ranges::any_of(group->files, is_path_protected)) {
            continue;  // a live download token (or other caller hold) on this group
        }
        for (const auto& file : group->files) {
            std::error_code rm_err;
            fs::remove(file, rm_err);
        }
        total -= group->bytes;
        freed += group->bytes;
        spdlog::info("ProxyRetention: evicted proxy group {} ({} bytes) — over the {} byte cap",
                     gid, group->bytes, max_total_bytes_);
    }
    return freed;
}

}  // namespace sst::adapters::raw_capture
