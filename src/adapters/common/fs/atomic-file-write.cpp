#include "adapters/common/fs/atomic-file-write.hpp"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace sst::adapters::fs_common {

namespace {

// Best-effort: fsync the directory containing `path` so the rename itself is
// durable. The data is already safe either way — a lost rename after a crash
// merely resurrects the previous complete file, never a partial one.
auto SyncParentDir(const std::string& path) -> void {
    const std::string dir = std::filesystem::path(path).parent_path().string();
    const int dir_fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return;
    }
    (void)::fsync(dir_fd);
    (void)::close(dir_fd);
}

}  // namespace

auto WriteFileAtomic(const std::string& path, std::string_view contents, mode_t mode) -> bool {
    const std::string tmp = path + ".tmp";
    // O_TRUNC discards a stale temp left by a previous crash. `mode` applies at
    // create time, so a restrictive mode (0600) holds from the first byte.
    const int file_fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (file_fd < 0) {
        spdlog::error("WriteFileAtomic: open {} failed: {}", tmp, std::strerror(errno));
        return false;
    }
    // A stale pre-existing temp keeps its old mode through O_CREAT; retighten.
    if (::fchmod(file_fd, mode) != 0) {
        spdlog::error("WriteFileAtomic: fchmod {} failed: {}", tmp, std::strerror(errno));
        (void)::close(file_fd);
        (void)::unlink(tmp.c_str());
        return false;
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t got = ::write(file_fd, contents.data() + written, contents.size() - written);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::error("WriteFileAtomic: write to {} failed: {}", tmp, std::strerror(errno));
            (void)::close(file_fd);
            (void)::unlink(tmp.c_str());
            return false;
        }
        written += static_cast<std::size_t>(got);
    }
    // fsync BEFORE rename: the new bytes must be on disk before they can become
    // the file at `path`, or a power cut could commit an empty rename target.
    const bool synced = ::fsync(file_fd) == 0;
    const bool closed = ::close(file_fd) == 0;
    if (!synced || !closed) {
        spdlog::error("WriteFileAtomic: fsync/close {} failed: {}", tmp, std::strerror(errno));
        (void)::unlink(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        spdlog::error("WriteFileAtomic: rename {} -> {} failed: {}", tmp, path,
                      std::strerror(errno));
        (void)::unlink(tmp.c_str());
        return false;
    }
    SyncParentDir(path);
    return true;
}

}  // namespace sst::adapters::fs_common
