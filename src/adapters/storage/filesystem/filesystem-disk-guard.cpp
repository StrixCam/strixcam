#include "adapters/storage/filesystem/filesystem-disk-guard.hpp"

#include <spdlog/spdlog.h>

#include <system_error>
#include <utility>

namespace sst::adapters::storage {

FilesystemDiskGuard::FilesystemDiskGuard(std::filesystem::path root,
                                         std::optional<std::uint64_t> min_free_bytes)
    : root_(std::move(root)), min_free_bytes_(min_free_bytes) {}

auto FilesystemDiskGuard::HasEnoughFreeSpace() const -> bool {
    if (!min_free_bytes_.has_value()) {
        return true;
    }
    const auto free = FreeBytes();
    const bool has_space = free >= *min_free_bytes_;
    if (!has_space) {
        spdlog::warn(
            "FilesystemDiskGuard: free={} bytes < min_free_bytes={} bytes at {} — refusing "
            "recording start",
            free, *min_free_bytes_, root_.string());
    }
    return has_space;
}

auto FilesystemDiskGuard::FreeBytes() const -> std::uint64_t {
    const auto target = ResolveExistingPath();
    if (target.empty()) {
        return 0;
    }
    std::error_code err;
    const auto info = std::filesystem::space(target, err);
    if (err) {
        spdlog::warn("FilesystemDiskGuard: std::filesystem::space({}) failed: {}", target.string(),
                     err.message());
        return 0;
    }
    return static_cast<std::uint64_t>(info.available);
}

auto FilesystemDiskGuard::ResolveExistingPath() const -> std::filesystem::path {
    std::error_code err;
    auto path = root_;
    while (!path.empty()) {
        if (std::filesystem::exists(path, err) && !err) {
            return path;
        }
        auto parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return {};
}

}  // namespace sst::adapters::storage
