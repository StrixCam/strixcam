// Download server: tokens, enumeration, and byte-range HTTP over loopback
// (U13, R24, F4).
//
// The token-mint/validate/expiry and enumeration tests are the in-container
// module-boundary coverage. The end-to-end HTTP test stands up a real
// cpp-httplib server + client on 127.0.0.1; it passes natively / on-device but
// is ENVIRONMENT-BOUND — qemu-user (the cross-compile container's aarch64
// emulator) does not run the threaded accept loop, so it fails here, like the
// other on-device e2e tests. It is committed and run for real on the device.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "adapters/network/http/http-download-server.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "domain/storage/models/raw-capture-identity.hpp"
#include "domain/storage/services/raw-capture-naming.hpp"
#include "httplib.h"

namespace fs = std::filesystem;

namespace {

using sst::network::DownloadServer;

constexpr std::uint64_t kTokenTtlSeconds = 60;
constexpr std::uint64_t kLongTokenTtlSeconds = 3600;
constexpr std::uint64_t kFixedNow = 1000;
constexpr std::uint64_t kFixedExpiry = kFixedNow + kTokenTtlSeconds;
constexpr std::uint64_t kStubClockNow = 100;
// Loopback accept-loop probe budget: kProbeAttempts × kProbeIntervalMs.
constexpr int kProbeAttempts = 100;
constexpr int kProbeIntervalMs = 20;

auto MakeRoot() -> fs::path {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("sst_dl_" + std::to_string(stamp) + "_" +
                                                 std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}

// Identifies a recording to write: the match name (used as the directory and
// file stem) and its file body. A struct keeps the two same-typed strings from
// being swapped at the call site.
struct RecordingFile {
    std::string match;
    std::string body;
};

auto WriteRecording(const fs::path& root, const RecordingFile& recording) -> void {
    const fs::path dir = root / "user" / recording.match;
    fs::create_directories(dir);
    std::ofstream out(dir / (recording.match + ".mp4"), std::ios::binary);
    out << recording.body;
}

// Write a raw dual-camera file under the naming convention.
auto WriteRawFile(const fs::path& root, const std::string& group, std::uint32_t camera_index,
                  const std::string& body) -> void {
    fs::create_directories(root);
    const auto name = sst::storage::raw_capture_naming::FileName(
        sst::storage::RawCaptureIdentity{.capture_group_id = group, .camera_index = camera_index});
    std::ofstream out(root / name, std::ios::binary);
    out << body;
}

// Write a <match>.jpg thumbnail under a per-match subdir of the thumbnail root,
// mirroring the firmware's /<user>/<match>/<match>.jpg layout.
auto WriteThumbnail(const fs::path& thumb_root, const RecordingFile& thumb) -> void {
    const fs::path dir = thumb_root / "user" / thumb.match;
    fs::create_directories(dir);
    std::ofstream out(dir / (thumb.match + ".jpg"), std::ios::binary);
    out << thumb.body;
}

// Enumeration finds MP4s on disk with their sizes.
TEST(DownloadServerTest, EnumeratesRecordings) {
    const fs::path root = MakeRoot();
    WriteRecording(root, {.match = "match-a", .body = "aaaa"});
    WriteRecording(root, {.match = "match-b", .body = "bbbbbb"});
    DownloadServer server(root, root, [] { return kStubClockNow; });

    auto recs = server.Enumerate();
    ASSERT_EQ(recs.size(), 2U);
    std::uint64_t total = 0;
    for (const auto& rec : recs) {
        total += rec.size_bytes;
        EXPECT_EQ(rec.thumbnail_id, rec.recording_id);
    }
    EXPECT_EQ(total, 10U);
    fs::remove_all(root);
}

// Raw capture files enumerate as a pair sharing capture_group_id with distinct
// camera_index + is_raw=true; final recordings stay is_raw=false. A raw file
// also resolves to a download token.
// gtest EXPECT_* macro expansion inflates cognitive complexity; the body is a
// linear arrange/act/assert, so the metric is suppressed here.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(DownloadServerTest, EnumeratesRawCapturePairAndFinalRecording) {
    const fs::path root = MakeRoot();
    WriteRecording(root, {.match = "match-final", .body = "final-bytes"});
    WriteRawFile(root, "grp-9", 0, "cam0-raw-bytes");
    WriteRawFile(root, "grp-9", 1, "cam1-raw-bytes");
    DownloadServer server(root, root, [] { return std::uint64_t{0}; });

    auto recs = server.Enumerate();
    ASSERT_EQ(recs.size(), 3U);

    int raw_count = 0;
    int final_count = 0;
    for (const auto& rec : recs) {
        if (rec.is_raw) {
            ++raw_count;
            EXPECT_EQ(rec.capture_group_id, "grp-9");
            EXPECT_TRUE(rec.camera_index == 0U || rec.camera_index == 1U);
        } else {
            ++final_count;
            EXPECT_EQ(rec.recording_id, "match-final");
            EXPECT_TRUE(rec.capture_group_id.empty());
        }
    }
    EXPECT_EQ(raw_count, 2);
    EXPECT_EQ(final_count, 1);

    // A raw file is downloadable too (token resolves by stem).
    auto token = server.MintToken("raw__grp-9__cam0", /*ttl_seconds=*/kTokenTtlSeconds);
    EXPECT_TRUE(token.has_value());

    fs::remove_all(root);
}
// NOLINTEND(readability-function-cognitive-complexity)

// ResolveThumbnailPath finds <id>.jpg under the thumbnail root by stem, and
// returns nullopt for an unknown id or an empty id. Pure filesystem logic —
// container-runnable (unlike the loopback HTTP test).
TEST(DownloadServerTest, ResolvesThumbnailByStem) {
    const fs::path video_root = MakeRoot();
    const fs::path thumb_root = MakeRoot();
    WriteThumbnail(thumb_root, {.match = "match-a", .body = "jpeg"});
    DownloadServer server(video_root, thumb_root, [] { return kStubClockNow; });

    auto path = server.ResolveThumbnailPath("match-a");
    if (!path) {
        FAIL() << "ResolveThumbnailPath returned nullopt";
        return;
    }
    EXPECT_EQ(path->filename().string(), "match-a.jpg");
    EXPECT_FALSE(server.ResolveThumbnailPath("ghost").has_value());
    EXPECT_FALSE(server.ResolveThumbnailPath("").has_value());

    fs::remove_all(video_root);
    fs::remove_all(thumb_root);
}

// A token validates until it expires; an unknown token never validates.
TEST(DownloadServerTest, TokenMintValidateExpire) {
    const fs::path root = MakeRoot();
    WriteRecording(root, {.match = "match-a", .body = "data"});
    std::uint64_t now = kFixedNow;
    DownloadServer server(root, root, [&now] { return now; });

    auto token = server.MintToken("match-a", /*ttl_seconds=*/kTokenTtlSeconds);
    if (!token) {
        FAIL() << "MintToken returned nullopt";
        return;
    }
    EXPECT_EQ(token->expires_at_unix, kFixedExpiry);
    EXPECT_TRUE(server.ValidateToken(token->token).has_value());

    EXPECT_FALSE(server.ValidateToken("bogus-token").has_value());

    now = kFixedExpiry;  // at expiry
    EXPECT_FALSE(server.ValidateToken(token->token).has_value());

    // A token for a non-existent recording is not minted.
    EXPECT_FALSE(server.MintToken("ghost", kTokenTtlSeconds).has_value());
    fs::remove_all(root);
}

// StreamFileRange contract (container-safe, no server): true only when the
// FULL requested range was delivered. httplib re-invokes its content provider
// at the same offset until the advertised length is served, so returning true
// on a short read (file truncated after its size was taken) would busy-spin the
// worker at the same offset forever. A short read must report failure so
// httplib aborts the response — the request terminates instead of looping.
TEST(DownloadServerTest, StreamFileRangeFailsOnTruncatedFile) {
    const fs::path root = MakeRoot();
    const fs::path file = root / "clip.mp4";
    const std::string body = "0123456789";  // 10 bytes on disk
    {
        std::ofstream out(file, std::ios::binary);
        out << body;
    }

    std::string received;
    httplib::DataSink sink;
    sink.write = [&received](const char* data, std::size_t len) {
        received.append(data, len);
        return true;
    };
    sink.done = [] {};
    sink.is_writable = [] { return true; };

    // Fully satisfiable range -> success, exact bytes.
    ASSERT_TRUE(sst::adapters::network::StreamFileRange(file.string(), 0, body.size(), sink));
    EXPECT_EQ(received, body);

    // Advertised length beyond EOF (truncated-file case) -> failure, and the
    // available suffix flowed exactly once (no re-invocation loop can restart
    // it, because false aborts the response).
    received.clear();
    constexpr std::size_t kOffset = 4;
    constexpr std::size_t kBeyondEofLength = 100;
    EXPECT_FALSE(
        sst::adapters::network::StreamFileRange(file.string(), kOffset, kBeyondEofLength, sink));
    EXPECT_EQ(received, body.substr(kOffset));

    // Missing file -> failure outright.
    EXPECT_FALSE(
        sst::adapters::network::StreamFileRange((root / "ghost.mp4").string(), 0, 1, sink));

    fs::remove_all(root);
}

// End-to-end over loopback: a minted token authorizes a full + ranged GET;
// missing/foreign tokens are rejected with 401. ENVIRONMENT-BOUND: passes
// natively / on-device, fails under the container's qemu-user (see file header).
// gtest EXPECT_* macro expansion inflates cognitive complexity; the body is a
// linear arrange/act/assert, so the metric is suppressed here.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(DownloadServerTest, HttpServesByteRangeWithBearerToken) {
    const fs::path root = MakeRoot();
    const fs::path thumb_root = MakeRoot();
    const std::string body = "0123456789ABCDEF";
    const std::string thumb_body = "\xFF\xD8\xFFjpeg-bytes";
    WriteRecording(root, {.match = "match-a", .body = body});
    WriteThumbnail(thumb_root, {.match = "match-a", .body = thumb_body});
    DownloadServer downloads(root, thumb_root, [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    });
    auto token = downloads.MintToken("match-a", kLongTokenTtlSeconds);
    if (!token) {
        FAIL() << "MintToken returned nullopt";
        return;
    }

    sst::adapters::network::HttpDownloadServer http(
        "127.0.0.1", 0,
        [&downloads](const std::string& bearer) { return downloads.ValidateToken(bearer); },
        [&downloads](const std::string& recording_id) {
            return downloads.ResolveThumbnailPath(recording_id);
        });
    ASSERT_TRUE(http.Start());
    const std::uint16_t port = http.BoundPort();
    ASSERT_GT(port, 0);

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);

    // Wait for the listener to accept connections (qemu scheduling can lag).
    httplib::Result probe;
    for (int i = 0; i < kProbeAttempts; ++i) {
        probe = client.Get("/recordings/match-a");
        if (probe) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kProbeIntervalMs));
    }

    // No token -> 401.
    {
        ASSERT_TRUE(probe) << "could not connect to loopback HTTP server";
        EXPECT_EQ(probe->status, 401);
    }
    // Foreign token -> 401.
    {
        auto res = client.Get("/recordings/match-a", {{"Authorization", "Bearer nope"}});
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 401);
    }
    // Valid token -> 200 + full body.
    {
        auto res = client.Get("/recordings/match-a", {{"Authorization", "Bearer " + token->token}});
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->body, body);
    }
    // Valid token + Range -> 206 + slice.
    {
        auto res = client.Get("/recordings/match-a", {{"Authorization", "Bearer " + token->token},
                                                      {"Range", "bytes=4-7"}});
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 206);
        EXPECT_EQ(res->body, "4567");
    }
    // Thumbnail: untokened GET by id -> 200 image/jpeg + bytes.
    {
        auto res = client.Get("/thumbnails/match-a");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->body, thumb_body);
        EXPECT_EQ(res->get_header_value("Content-Type"), "image/jpeg");
    }
    // Unknown thumbnail id -> 404.
    {
        auto res = client.Get("/thumbnails/ghost");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 404);
    }

    http.Stop();
    fs::remove_all(root);
    fs::remove_all(thumb_root);
}
// NOLINTEND(readability-function-cognitive-complexity)

}  // namespace
