// DownloadHandler ListRecordings: the wire list carries app-visible recordings
// only — internal proxy files never cross to proto RecordingMetadata (their
// marker fields are retired + reserved in the contract).

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
#include "domain/storage/models/proxy-identity.hpp"
#include "domain/storage/services/proxy-naming.hpp"

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

// The internal proxy is invisible to the app: a proxy pair sitting beside the
// final MP4 in the same match folder never reaches the wire list, and only the
// final recording is reported.
TEST(DownloadHandlerTest, ListExcludesInternalProxyFiles) {
    const fs::path root = MakeRoot();
    WriteFile(root / "match-x.mp4", "final");
    namespace naming = sst::storage::proxy_naming;
    WriteFile(root / naming::FileName({.match_uuid = "match-x", .camera_index = 0}), "c0");
    WriteFile(root / naming::FileName({.match_uuid = "match-x", .camera_index = 1}), "c1");

    constexpr std::uint32_t kDownloadPort = 8080;
    constexpr std::uint64_t kTokenTtlSeconds = 3600;
    DownloadServer server(root, root, [] { return std::uint64_t{0}; });
    DownloadHandler handler(server, "192.168.49.1", kDownloadPort,
                            /*token_ttl_seconds=*/kTokenTtlSeconds);

    auto resp = handler.Handle(ListCmd());
    ASSERT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kRecordingList);

    ASSERT_EQ(resp.recording_list().recordings_size(), 1);
    EXPECT_EQ(resp.recording_list().recordings(0).id(), "match-x");

    fs::remove_all(root);
}

}  // namespace
