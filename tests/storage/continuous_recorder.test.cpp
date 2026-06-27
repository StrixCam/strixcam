// RecordingService over the continuous recorder (U10, R14/R21, AE2/AE4).
// Pure — fake recorder + thumbnail writer + disk guard. The real NVENC encode
// is covered by a hardware-bound test.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/storage/ports/continuous-recorder.hpp"
#include "app/storage/ports/disk-guard.hpp"
#include "app/storage/ports/thumbnail-writer.hpp"
#include "app/storage/services/recording_service/recording-service.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/storage/models/recording-state.hpp"

namespace fs = std::filesystem;

namespace {

using sst::storage::RecordingService;
using sst::storage::RecordingState;

class FakeRecorder final : public sst::storage::IContinuousRecorder {
   public:
    auto Start(const fs::path& output) -> bool override {
        started_path = output;
        running = true;
        ++starts;
        return start_ok;
    }
    auto Pause() -> void override { paused = true; }
    auto Resume() -> void override { paused = false; }
    auto Stop() -> bool override {
        running = false;
        ++stops;
        return true;
    }
    [[nodiscard]] auto IsRunning() const -> bool override { return running; }
    auto Push(const sst::capture::Frame& /*frame*/) -> void override { ++pushes; }

    bool start_ok{true};
    bool running{false};
    bool paused{false};
    int starts{0};
    int stops{0};
    int pushes{0};
    fs::path started_path;
};

class FakeThumbnailWriter final : public sst::storage::IThumbnailWriter {
   public:
    auto Write(const sst::capture::Frame& /*frame*/, const fs::path& path) -> bool override {
        ++writes;
        written_path = path;
        return true;
    }
    int writes{0};
    fs::path written_path;
};

class FakeDiskGuard final : public sst::storage::IDiskGuard {
   public:
    [[nodiscard]] auto HasEnoughFreeSpace() const -> bool override { return has_space; }
    [[nodiscard]] auto FreeBytes() const -> std::uint64_t override { return kFakeFreeBytes; }
    bool has_space{true};

   private:
    static constexpr std::uint64_t kFakeFreeBytes = 1000;
};

auto MakeFrame() -> sst::capture::Frame {
    // A tiny 2x2 BGR frame (stride = 2 px * 3 channels = 6 bytes) filled with an
    // arbitrary sentinel byte; the fakes never inspect the pixels.
    constexpr std::uint32_t kDim = 2;
    constexpr std::uint32_t kStride = kDim * 3;
    constexpr std::size_t kBufBytes = static_cast<std::size_t>(kStride) * kDim;
    constexpr std::uint8_t kFill = 0xAB;
    static std::vector<std::uint8_t> buf(kBufBytes, kFill);
    sst::capture::Frame frame;
    frame.geometry = {.width = kDim, .height = kDim};
    frame.format = sst::common::PixelFormat::BGR8;
    frame.planes.push_back({.stride = kStride, .data = buf.data(), .size = buf.size()});
    return frame;
}

struct Svc {
    FakeRecorder* recorder;
    FakeThumbnailWriter* thumb;
    FakeDiskGuard guard;
    std::unique_ptr<RecordingService> service;

    Svc() {
        auto rec = std::make_unique<FakeRecorder>();
        auto thm = std::make_unique<FakeThumbnailWriter>();
        recorder = rec.get();
        thumb = thm.get();
        service = std::make_unique<RecordingService>(std::move(rec), std::move(thm), guard);
    }
};

constexpr const char* kVideo = "/tmp/sst-rec/user/match/match.mp4";
constexpr const char* kThumb = "/tmp/sst-rec/thumb/user/match/match.jpg";

// The app/session hands a per-match DIRECTORY (trailing slash), not a file.
// RecordingService must compose a concrete file inside it before handing it to
// the recorder (filesink) / thumbnail writer (imwrite) — both fail on a bare
// directory, which left empty match dirs on-device. Guards that regression.
TEST(ContinuousRecorderTest, DirectoryContractComposesConcreteFiles) {
    Svc harness;
    constexpr const char* kVideoDir = "/tmp/sst-rec/dir/user/match/";
    constexpr const char* kThumbDir = "/tmp/sst-rec/dir/thumb/user/match/";

    ASSERT_TRUE(harness.service->StartRecording(kVideoDir, kThumbDir));
    EXPECT_EQ(harness.recorder->started_path.string(), "/tmp/sst-rec/dir/user/match/l1.mp4");

    harness.service->Push(MakeFrame());  // gives Stop a frame to thumbnail
    const auto result = harness.service->Stop();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.file_path.string(), "/tmp/sst-rec/dir/user/match/l1.mp4");
    EXPECT_EQ(harness.thumb->written_path.string(), "/tmp/sst-rec/dir/thumb/user/match/l1.jpg");
}

// AE4 / R21: START -> PAUSE -> RESUME -> STOP yields a single file at the
// contract path (one recorder Start, one Stop — never multiple segments).
TEST(ContinuousRecorderTest, StartPauseResumeStopIsSingleFile) {
    Svc harness;
    ASSERT_TRUE(harness.service->StartRecording(kVideo, kThumb));
    EXPECT_EQ(harness.service->CurrentState(), RecordingState::kRecording);
    EXPECT_EQ(harness.recorder->started_path.string(), kVideo);

    EXPECT_TRUE(harness.service->Pause());
    EXPECT_EQ(harness.service->CurrentState(), RecordingState::kPaused);
    EXPECT_TRUE(harness.service->Resume());
    EXPECT_EQ(harness.service->CurrentState(), RecordingState::kRecording);

    auto result = harness.service->Stop();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.file_path.string(), kVideo);
    EXPECT_EQ(harness.recorder->starts, 1);  // single file
    EXPECT_EQ(harness.recorder->stops, 1);
    EXPECT_EQ(harness.service->CurrentState(), RecordingState::kIdle);
}

// Thumbnail is written at finalization when a frame has been seen.
TEST(ContinuousRecorderTest, ThumbnailWrittenAtFinalize) {
    Svc harness;
    ASSERT_TRUE(harness.service->StartRecording(kVideo, kThumb));
    harness.service->Push(MakeFrame());
    auto result = harness.service->Stop();
    EXPECT_TRUE(result.thumbnail_written);
    EXPECT_EQ(harness.thumb->writes, 1);
    EXPECT_EQ(harness.thumb->written_path.string(), kThumb);
}

// AE2 / R14: disconnect-finalize (Stop without an explicit STOP command) still
// EOSes the muxer to a playable file.
TEST(ContinuousRecorderTest, DisconnectFinalizeStops) {
    Svc harness;
    ASSERT_TRUE(harness.service->StartRecording(kVideo, kThumb));
    harness.service->Push(MakeFrame());
    // Simulate disconnect cleanup calling Stop() directly.
    auto result = harness.service->Stop();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(harness.recorder->stops, 1);
    EXPECT_EQ(harness.service->CurrentState(), RecordingState::kIdle);
}

// Stop while idle is a harmless no-op (idempotent cleanup path).
TEST(ContinuousRecorderTest, StopWhenIdleIsNoop) {
    Svc harness;
    auto result = harness.service->Stop();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(harness.recorder->stops, 0);
}

// Disk-full blocks START with a defined failure.
TEST(ContinuousRecorderTest, DiskFullBlocksStart) {
    Svc harness;
    harness.guard.has_space = false;
    // Rebuild service so it captures the disk-full guard.
    auto recorder = std::make_unique<FakeRecorder>();
    auto thumb = std::make_unique<FakeThumbnailWriter>();
    auto* rec = recorder.get();
    RecordingService svc(std::move(recorder), std::move(thumb), harness.guard);
    EXPECT_FALSE(svc.StartRecording(kVideo, kThumb));
    EXPECT_EQ(rec->starts, 0);
    EXPECT_EQ(svc.CurrentState(), RecordingState::kIdle);
}

// Frames are only pushed to the recorder while recording.
TEST(ContinuousRecorderTest, FramesPushedOnlyWhileActive) {
    Svc harness;
    harness.service->Push(MakeFrame());  // idle -> ignored
    EXPECT_EQ(harness.recorder->pushes, 0);
    ASSERT_TRUE(harness.service->StartRecording(kVideo, kThumb));
    harness.service->Push(MakeFrame());
    EXPECT_EQ(harness.recorder->pushes, 1);
}

}  // namespace
