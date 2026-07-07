#include <gst/gst.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "adapters/common/gstreamer/gst-frame.hpp"
#include "adapters/storage/proxy/filesystem-proxy-sink.hpp"
#include "app/storage/ports/proxy-sink.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/storage/models/proxy-identity.hpp"
#include "domain/storage/services/proxy-naming.hpp"

// The internal-proxy sink runs a per-camera GStreamer H.264 encode pipeline
// (appsrc -> nvvidconv/x264enc -> mp4mux -> filesink). Tests that call Start()
// therefore instantiate real GStreamer elements and only pass on-device, so they
// live in the ProxySinkE2E suite (excluded from the container run by name, same
// as the other hardware-bound suites). The pure state-machine guards below
// (empty-match reject, push-when-idle no-op, stop-without-start) short-circuit
// BEFORE building any pipeline, so they run in the container.

namespace {

using sst::adapters::storage::FilesystemProxySink;
using sst::capture::Frame;

// R17: the internal proxy is NOT quality-controllable — record/stream quality
// must never reach it. The sink's Start takes only the match id + output dir;
// pinned at compile time so a future signature change that adds a quality knob
// to the proxy path fails the build.
static_assert(std::is_same_v<decltype(&sst::storage::IProxySink::Start),
                             bool (sst::storage::IProxySink::*)(const std::string&,
                                                                const std::filesystem::path&)>,
              "proxy Start takes a match id + output dir, no quality knob (R17)");

// A unique temp directory per test, removed on destruction.
class TempDir {
   public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() /
                ("sst-proxy-" + tag + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code err;
        std::filesystem::remove_all(path_, err);
    }
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

   private:
    std::filesystem::path path_;
};

// A valid TWO-PLANE NV12 frame (Y + interleaved UV), like the real capture
// pipeline produces — a single-plane synthetic frame would mask a sink that
// pushes only planes[0] and drops the chroma plane. Total == width*height*3/2
// so the appsrc caps (set from geometry) match the buffer the sink fills.
constexpr std::uint32_t kFrameDim = 64;
constexpr std::size_t kYPlaneSize = static_cast<std::size_t>(kFrameDim) * kFrameDim;
constexpr std::size_t kUvPlaneSize = kYPlaneSize / 2;
constexpr std::size_t kNv12FullSize = kYPlaneSize + kUvPlaneSize;

auto MakeFrame(std::uint8_t fill) -> Frame {
    auto owner = std::make_shared<std::vector<std::uint8_t>>(kNv12FullSize, fill);
    Frame frame;
    frame.format = sst::common::PixelFormat::NV12;
    frame.geometry = {.width = kFrameDim, .height = kFrameDim};
    frame.planes.push_back(
        sst::capture::FramePlane{.stride = kFrameDim, .data = owner->data(), .size = kYPlaneSize});
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = kFrameDim, .data = owner->data() + kYPlaneSize, .size = kUvPlaneSize});
    frame.owner = owner;
    return frame;
}

auto ProxyPath(const std::filesystem::path& dir,
               std::uint32_t camera_index) -> std::filesystem::path {
    namespace naming = sst::storage::proxy_naming;
    return dir / naming::FileName({.match_uuid = "match-1", .camera_index = camera_index});
}

// ---- Container-safe: pure state-machine guards (no pipeline built) ----------

TEST(ProxySinkTest, EmptyMatchUuidRejected) {
    TempDir dir("empty");
    FilesystemProxySink sink(dir.path(), 2);
    EXPECT_FALSE(sink.Start("", dir.path()));  // rejected before any GStreamer work
    EXPECT_FALSE(sink.IsCapturing());
}

TEST(ProxySinkTest, PushWhenNotCapturingIsNoOp) {
    TempDir dir("idle");
    FilesystemProxySink sink(dir.path(), 2);
    constexpr std::uint32_t kOutOfRangeCamera = 5;
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident NV12 fill byte
    sink.PushCamera(0, MakeFrame(0x33));  // no Start(): harmless no-op
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident NV12 fill byte
    sink.PushCamera(kOutOfRangeCamera, MakeFrame(0x33));  // out-of-range index
    EXPECT_FALSE(sink.Stop());                            // nothing to stop
}

// UV-plane regression: the sink pushes frames via the shared frame→GstBuffer
// helper, which must carry the FULL frame — both NV12 planes — not just
// planes[0] (the bug that chroma-broke every proxy MP4 on device). Container-
// safe: allocates a GstBuffer, builds no pipeline.
TEST(ProxySinkTest, FrameToGstBufferCarriesBothNv12Planes) {
    gst_init(nullptr, nullptr);
    const Frame frame = MakeFrame(0x11);  // NOLINT(readability-magic-numbers) — NV12 fill byte
    ASSERT_EQ(frame.planes.size(), 2U);
    EXPECT_EQ(sst::adapters::gst_common::FrameByteSize(frame), kNv12FullSize);

    GstBuffer* buffer = sst::adapters::gst_common::MakeGstBufferFromFrame(frame);
    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(gst_buffer_get_size(buffer), kNv12FullSize);  // Y + UV, not Y alone
    gst_buffer_unref(buffer);
}

// ---- On-device (ProxySinkE2E): real GStreamer encode pipeline ---------------

// The proxy pair is NAMED BY THE MATCH UUID (proxy__<match>__cam<N>.mp4) in the
// given per-match dir — the on-device pairing contract (retrieval by match id).
TEST(ProxySinkE2E, StartPushStopWritesBothProxyFilesNamedByMatch) {
    constexpr std::uint32_t kCameraCount = 2;
    constexpr int kFramesPerCamera = 10;

    TempDir dir("both");
    FilesystemProxySink sink(dir.path(), kCameraCount);

    ASSERT_TRUE(sink.Start("match-1", dir.path()));
    EXPECT_TRUE(sink.IsCapturing());

    for (int i = 0; i < kFramesPerCamera; ++i) {
        // NOLINTNEXTLINE(readability-magic-numbers) — self-evident NV12 fill byte
        sink.PushCamera(0, MakeFrame(0x11));
        // NOLINTNEXTLINE(readability-magic-numbers) — self-evident NV12 fill byte
        sink.PushCamera(1, MakeFrame(0x22));
    }

    EXPECT_TRUE(sink.Stop());  // EOS + moov finalize
    EXPECT_FALSE(sink.IsCapturing());

    // One playable H.264 MP4 per camera at the naming-convention path.
    EXPECT_TRUE(std::filesystem::exists(ProxyPath(dir.path(), 0)));
    EXPECT_TRUE(std::filesystem::exists(ProxyPath(dir.path(), 1)));
}

TEST(ProxySinkE2E, StartWhileCapturingIsRejected) {
    TempDir dir("twice");
    FilesystemProxySink sink(dir.path(), 2);
    ASSERT_TRUE(sink.Start("m", dir.path()));
    EXPECT_FALSE(sink.Start("m2", dir.path()));  // already capturing
    sink.Stop();
}

TEST(ProxySinkE2E, StopIsIdempotentlySafe) {
    TempDir dir("stop2");
    FilesystemProxySink sink(dir.path(), 2);
    ASSERT_TRUE(sink.Start("m", dir.path()));
    EXPECT_TRUE(sink.Stop());
    EXPECT_FALSE(sink.Stop());  // second stop is a no-op false
}

}  // namespace
