// Training-proxy retention sweep (U7). Pure filesystem — no GStreamer/hardware.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/raw_capture/proxy-retention.hpp"
#include "domain/raw_capture/models/raw-capture-identity.hpp"
#include "domain/raw_capture/services/raw-capture-naming.hpp"

namespace {

namespace fs = std::filesystem;
using sst::adapters::raw_capture::ProxyRetention;
namespace naming = sst::raw_capture::raw_capture_naming;

class TempDir {
   public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("sst-retention-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code err;
        fs::remove_all(path_, err);
    }
    [[nodiscard]] auto path() const -> const fs::path& { return path_; }

   private:
    fs::path path_;
};

// Write both camera files of a proxy group, each `bytes` big, with a mtime
// `age_seconds` in the past (so the write-grace doesn't protect old groups).
void MakeGroup(const fs::path& dir, const std::string& group, std::size_t bytes, int age_seconds) {
    for (std::uint32_t cam = 0; cam < 2; ++cam) {
        const auto path =
            dir / naming::FileName({.capture_group_id = group, .camera_index = cam});
        std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
        fs::last_write_time(path,
                            fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds));
    }
}

auto GroupExists(const fs::path& dir, const std::string& group) -> bool {
    return fs::exists(dir / naming::FileName({.capture_group_id = group, .camera_index = 0}));
}

constexpr std::size_t kPerFile = 100;  // 200 bytes per 2-camera group

TEST(ProxyRetentionTest, EvictsOldestOverCapKeepsNewestAndRecent) {
    TempDir dir;
    MakeGroup(dir.path(), "old", kPerFile, /*age=*/300);
    MakeGroup(dir.path(), "mid", kPerFile, /*age=*/120);
    MakeGroup(dir.path(), "recent", kPerFile, /*age=*/0);  // within write-grace

    // Total 600; cap 450 => must free ~150 => evict the single oldest group.
    ProxyRetention retention(dir.path(), /*max_total_bytes=*/450);
    const auto freed = retention.Sweep([](const fs::path&) { return false; });

    EXPECT_EQ(freed, 200U);
    EXPECT_FALSE(GroupExists(dir.path(), "old"));    // oldest evicted
    EXPECT_TRUE(GroupExists(dir.path(), "mid"));     // within cap now
    EXPECT_TRUE(GroupExists(dir.path(), "recent"));  // grace-protected regardless
}

TEST(ProxyRetentionTest, NeverEvictsGraceProtectedActiveGroup) {
    TempDir dir;
    MakeGroup(dir.path(), "recent", kPerFile, /*age=*/0);  // actively writing

    // Way over a tiny cap, but the only group is within the write-grace.
    ProxyRetention retention(dir.path(), /*max_total_bytes=*/10);
    EXPECT_EQ(retention.Sweep([](const fs::path&) { return false; }), 0U);
    EXPECT_TRUE(GroupExists(dir.path(), "recent"));
}

TEST(ProxyRetentionTest, SkipsGroupsWithALiveDownloadToken) {
    TempDir dir;
    MakeGroup(dir.path(), "tokened", kPerFile, /*age=*/300);
    MakeGroup(dir.path(), "free", kPerFile, /*age=*/120);

    // Cap forces an eviction; protect the OLDER (tokened) group — the sweep must
    // skip it and evict the other instead.
    ProxyRetention retention(dir.path(), /*max_total_bytes=*/250);
    const auto protectedFile =
        dir.path() / naming::FileName({.capture_group_id = "tokened", .camera_index = 0});
    retention.Sweep([&](const fs::path& f) { return f == protectedFile; });

    EXPECT_TRUE(GroupExists(dir.path(), "tokened"));  // protected by "token"
    EXPECT_FALSE(GroupExists(dir.path(), "free"));    // evicted instead
}

TEST(ProxyRetentionTest, DisabledWhenCapIsZero) {
    TempDir dir;
    MakeGroup(dir.path(), "old", kPerFile, /*age=*/300);
    ProxyRetention retention(dir.path(), /*max_total_bytes=*/0);
    EXPECT_EQ(retention.Sweep([](const fs::path&) { return false; }), 0U);
    EXPECT_TRUE(GroupExists(dir.path(), "old"));
}

}  // namespace
