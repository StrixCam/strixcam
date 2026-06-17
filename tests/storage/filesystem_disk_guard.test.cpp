#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include "adapters/storage/filesystem/filesystem-disk-guard.hpp"

namespace {

using sst::adapters::storage::FilesystemDiskGuard;

auto NextSuffix() -> std::string {
    static std::atomic<int> counter{0};
    return std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
}

class FilesystemDiskGuardTest : public ::testing::Test {
   protected:
    auto SetUp() -> void override {
        root_ = std::filesystem::path(SST_REPO_ROOT_DIR) /
                ("tests/storage/data/disk_guard_" + NextSuffix());
        std::error_code err;
        std::filesystem::remove_all(root_, err);
        std::filesystem::create_directories(root_);
    }

    auto TearDown() -> void override {
        std::error_code err;
        std::filesystem::remove_all(root_, err);
    }

    std::filesystem::path root_;
};

TEST_F(FilesystemDiskGuardTest, NoThresholdAlwaysPasses) {
    FilesystemDiskGuard guard(root_, std::nullopt);
    EXPECT_TRUE(guard.HasEnoughFreeSpace());
}

TEST_F(FilesystemDiskGuardTest, ZeroThresholdAlwaysPasses) {
    FilesystemDiskGuard guard(root_, std::uint64_t{0});
    EXPECT_TRUE(guard.HasEnoughFreeSpace());
}

TEST_F(FilesystemDiskGuardTest, BelowThresholdReturnsFalse) {
    // No filesystem on the planet has 1 EiB free. The 60 is the exbibyte exponent
    // (2^60 bytes), self-evident from the constant's name.
    constexpr std::uint64_t kOneExbibyte = std::uint64_t{1}
                                           << 60U;  // NOLINT(readability-magic-numbers)
    FilesystemDiskGuard guard(root_, kOneExbibyte);
    EXPECT_FALSE(guard.HasEnoughFreeSpace());
}

TEST_F(FilesystemDiskGuardTest, FreeBytesNonZeroForRealPath) {
    FilesystemDiskGuard guard(root_, std::nullopt);
    EXPECT_GT(guard.FreeBytes(), 0U);
}

TEST_F(FilesystemDiskGuardTest, ResolvesParentWhenPathDoesNotExist) {
    // Construct the guard with a path that hasn't been created yet — simulates
    // first-ever recording on a fresh device. The guard must walk up to the
    // parent and report space there instead of failing on stat.
    const auto fresh_path = root_ / "deeply" / "nested" / "not_yet_created";
    EXPECT_FALSE(std::filesystem::exists(fresh_path));

    FilesystemDiskGuard guard(fresh_path, std::nullopt);
    EXPECT_GT(guard.FreeBytes(), 0U);
    EXPECT_TRUE(guard.HasEnoughFreeSpace());
}

TEST_F(FilesystemDiskGuardTest, EmptyPathReturnsZeroFreeBytes) {
    FilesystemDiskGuard guard(std::filesystem::path{}, std::nullopt);
    EXPECT_EQ(guard.FreeBytes(), 0U);
}

}  // namespace
