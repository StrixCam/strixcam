#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "adapters/storage/raw_capture/filesystem-raw-capture-sink.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/storage/models/raw-capture-identity.hpp"
#include "domain/storage/services/raw-capture-naming.hpp"

namespace {

using sst::adapters::storage::FilesystemRawCaptureSink;
using sst::capture::Frame;

// A unique temp directory per test, removed on destruction.
class TempDir {
   public:
    explicit TempDir(const std::string& tag) {
        path_ =
            std::filesystem::temp_directory_path() /
            ("sst-rawcap-" + tag + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// Build a Frame whose single plane points into an owned byte buffer (NV12-ish).
// The shared owner keeps the bytes alive for any copies the sink's ring holds.
auto MakeFrame(std::uint8_t fill, std::size_t size) -> Frame {
    // 16x16 is an arbitrary small geometry; the sink concatenates raw planes and
    // never interprets width/height, so any non-zero dimensions suffice.
    constexpr std::uint32_t kDim = 16;
    auto owner = std::make_shared<std::vector<std::uint8_t>>(size, fill);
    Frame frame;
    frame.geometry = {.width = kDim, .height = kDim};
    frame.planes.push_back(
        sst::capture::FramePlane{.stride = kDim, .data = owner->data(), .size = owner->size()});
    frame.owner = owner;
    return frame;
}

auto FileSize(const std::filesystem::path& path) -> std::uintmax_t {
    std::error_code err;
    const auto size = std::filesystem::file_size(path, err);
    return err ? 0 : size;
}

// Assert one per-camera raw file exists at the naming-convention path and holds
// exactly the bytes of every pushed frame (no drop, no truncation). Extracted to
// keep the test body's cognitive complexity in check. `expected_bytes` precedes
// `camera_index` so the two integral parameters are not adjacent/swappable.
void ExpectCameraFile(std::uintmax_t expected_bytes, const std::filesystem::path& dir,
                      std::uint32_t camera_index) {
    namespace naming = sst::storage::raw_capture_naming;
    const auto path =
        dir / naming::FileName({.capture_group_id = "grp-1", .camera_index = camera_index});
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(FileSize(path), expected_bytes);
}

TEST(RawCaptureSinkTest, StartPushStopWritesBothCameraFiles) {
    constexpr std::uint32_t kCameraCount = 2;
    constexpr std::uint8_t kCam0Fill = 0x11;
    constexpr std::uint8_t kCam1Fill = 0x22;
    constexpr std::size_t kFrameBytes = 256;
    constexpr int kFramesPerCamera = 5;
    constexpr std::chrono::milliseconds kDrainWait{50};

    TempDir dir("both");
    FilesystemRawCaptureSink sink(dir.path(), kCameraCount);

    ASSERT_TRUE(sink.Start("grp-1"));
    EXPECT_TRUE(sink.IsCapturing());

    for (int i = 0; i < kFramesPerCamera; ++i) {
        sink.PushCamera(0, MakeFrame(kCam0Fill, kFrameBytes));
        sink.PushCamera(1, MakeFrame(kCam1Fill, kFrameBytes));
    }
    // Give the writer threads time to drain before Stop (Stop also drains, but
    // this exercises the steady-state path).
    std::this_thread::sleep_for(kDrainWait);

    EXPECT_TRUE(sink.Stop());
    EXPECT_FALSE(sink.IsCapturing());

    // Drop-oldest may discard frames under load, but with a tiny payload and the
    // drain on Stop every pushed frame should land: kFramesPerCamera * kFrameBytes.
    const std::uintmax_t expected = std::uintmax_t{kFramesPerCamera} * kFrameBytes;
    ExpectCameraFile(expected, dir.path(), 0);
    ExpectCameraFile(expected, dir.path(), 1);
}

TEST(RawCaptureSinkTest, StartWhileCapturingIsRejected) {
    TempDir dir("twice");
    FilesystemRawCaptureSink sink(dir.path(), 2);
    ASSERT_TRUE(sink.Start("g"));
    EXPECT_FALSE(sink.Start("g2"));  // already capturing
    sink.Stop();
}

TEST(RawCaptureSinkTest, EmptyGroupIdRejected) {
    TempDir dir("empty");
    FilesystemRawCaptureSink sink(dir.path(), 2);
    EXPECT_FALSE(sink.Start(""));
    EXPECT_FALSE(sink.IsCapturing());
}

TEST(RawCaptureSinkTest, PushWhenNotCapturingIsNoOp) {
    TempDir dir("idle");
    FilesystemRawCaptureSink sink(dir.path(), 2);
    // No Start(): these must be harmless no-ops, not crashes.
    constexpr std::uint8_t kFill = 0x33;
    constexpr std::size_t kBytes = 64;
    constexpr std::uint32_t kOutOfRangeCamera = 5;
    sink.PushCamera(0, MakeFrame(kFill, kBytes));
    sink.PushCamera(kOutOfRangeCamera, MakeFrame(kFill, kBytes));  // out-of-range index
    EXPECT_FALSE(sink.Stop());                                     // nothing to stop
}

TEST(RawCaptureSinkTest, StopIsIdempotentlySafe) {
    TempDir dir("stop2");
    FilesystemRawCaptureSink sink(dir.path(), 2);
    ASSERT_TRUE(sink.Start("g"));
    EXPECT_TRUE(sink.Stop());
    EXPECT_FALSE(sink.Stop());  // second stop is a no-op false
}

}  // namespace
