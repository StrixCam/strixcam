// DownloadHandler ListRecordings: raw dual-camera identity fields cross to the
// proto RecordingMetadata under the joint invariant (U7).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/control/services/handlers/download.handler.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "bluetooth.pb.h"
#include "domain/storage/models/raw-capture-identity.hpp"
#include "domain/storage/services/raw-capture-naming.hpp"

namespace fs = std::filesystem;

namespace {

using sst::control::DownloadHandler;
using sst::network::DownloadServer;

auto MakeRoot() -> fs::path {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("sst_dlh_" + std::to_string(stamp) + "_" +
                                                 std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}

auto WriteFile(const fs::path& path, const std::string& body) -> void {
    std::ofstream out(path, std::ios::binary);
    out << body;
}

auto ListCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("l");
    cmd.mutable_list_recordings();
    return cmd;
}

// Joint invariant for a raw recording: both identity fields present.
void ExpectRawIdentity(const sst_cam::RecordingMetadata& meta) {
    EXPECT_TRUE(meta.has_camera_index());
    EXPECT_TRUE(meta.has_capture_group_id());
    EXPECT_EQ(meta.capture_group_id(), "grp-3");
}

// Joint invariant for a final recording: raw identity entirely absent.
void ExpectNoRawIdentity(const sst_cam::RecordingMetadata& meta) {
    EXPECT_FALSE(meta.has_is_raw());
    EXPECT_FALSE(meta.has_camera_index());
    EXPECT_FALSE(meta.has_capture_group_id());
}

TEST(DownloadHandlerTest, ListReportsRawIdentityUnderJointInvariant) {
    const fs::path root = MakeRoot();
    WriteFile(root / "match-x.mp4", "final");
    namespace naming = sst::storage::raw_capture_naming;
    WriteFile(root / naming::FileName({.capture_group_id = "grp-3", .camera_index = 0}), "c0");
    WriteFile(root / naming::FileName({.capture_group_id = "grp-3", .camera_index = 1}), "c1");

    constexpr std::uint32_t kDownloadPort = 8080;
    constexpr std::uint64_t kTokenTtlSeconds = 3600;
    DownloadServer server(root, root, [] { return std::uint64_t{0}; });
    DownloadHandler handler(server, "192.168.49.1", kDownloadPort,
                            /*token_ttl_seconds=*/kTokenTtlSeconds);

    auto resp = handler.Handle(ListCmd());
    ASSERT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kRecordingList);

    int raw = 0;
    int finals = 0;
    for (const auto& meta : resp.recording_list().recordings()) {
        if (meta.is_raw()) {
            ++raw;
            ExpectRawIdentity(meta);
        } else {
            ++finals;
            ExpectNoRawIdentity(meta);
        }
    }
    EXPECT_EQ(raw, 2);
    EXPECT_EQ(finals, 1);

    fs::remove_all(root);
}

}  // namespace
