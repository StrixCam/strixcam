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
    auto Push(const sst::capture::Frame& /*frame*/) -> void override {
      ++pushes;
    }

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
    auto Write(const sst::capture::Frame& /*frame*/, const fs::path& path)
        -> bool override {
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
    [[nodiscard]] auto FreeBytes() const -> std::uint64_t override { return 1000; }
    bool has_space{true};
};

auto MakeFrame() -> sst::capture::Frame {
    static std::vector<std::uint8_t> buf(16, 0xAB);
    sst::capture::Frame f;
    f.geometry = {.width = 2, .height = 2};
    f.format = sst::common::PixelFormat::BGR8;
    f.planes.push_back({.stride = 6, .data = buf.data(), .size = buf.size()});
    return f;
}

struct Svc {
    FakeRecorder* recorder;
    FakeThumbnailWriter* thumb;
    FakeDiskGuard guard;
    std::unique_ptr<RecordingService> service;

    Svc() {
        auto r = std::make_unique<FakeRecorder>();
        auto t = std::make_unique<FakeThumbnailWriter>();
        recorder = r.get();
        thumb = t.get();
        service = std::make_unique<RecordingService>(std::move(r), std::move(t), guard);
    }
};

constexpr const char* kVideo = "/tmp/sst-rec/user/match/match.mp4";
constexpr const char* kThumb = "/tmp/sst-rec/thumb/user/match/match.jpg";

// AE4 / R21: START -> PAUSE -> RESUME -> STOP yields a single file at the
// contract path (one recorder Start, one Stop — never multiple segments).
TEST(ContinuousRecorderTest, StartPauseResumeStopIsSingleFile) {
    Svc s;
    ASSERT_TRUE(s.service->StartRecording(kVideo, kThumb));
    EXPECT_EQ(s.service->CurrentState(), RecordingState::kRecording);
    EXPECT_EQ(s.recorder->started_path.string(), kVideo);

    EXPECT_TRUE(s.service->Pause());
    EXPECT_EQ(s.service->CurrentState(), RecordingState::kPaused);
    EXPECT_TRUE(s.service->Resume());
    EXPECT_EQ(s.service->CurrentState(), RecordingState::kRecording);

    auto result = s.service->Stop();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.file_path.string(), kVideo);
    EXPECT_EQ(s.recorder->starts, 1);  // single file
    EXPECT_EQ(s.recorder->stops, 1);
    EXPECT_EQ(s.service->CurrentState(), RecordingState::kIdle);
}

// Thumbnail is written at finalization when a frame has been seen.
TEST(ContinuousRecorderTest, ThumbnailWrittenAtFinalize) {
    Svc s;
    ASSERT_TRUE(s.service->StartRecording(kVideo, kThumb));
    s.service->Push(MakeFrame());
    auto result = s.service->Stop();
    EXPECT_TRUE(result.thumbnail_written);
    EXPECT_EQ(s.thumb->writes, 1);
    EXPECT_EQ(s.thumb->written_path.string(), kThumb);
}

// AE2 / R14: disconnect-finalize (Stop without an explicit STOP command) still
// EOSes the muxer to a playable file.
TEST(ContinuousRecorderTest, DisconnectFinalizeStops) {
    Svc s;
    ASSERT_TRUE(s.service->StartRecording(kVideo, kThumb));
    s.service->Push(MakeFrame());
    // Simulate disconnect cleanup calling Stop() directly.
    auto result = s.service->Stop();
    EXPECT_TRUE(result.success);
    EXPECT_EQ(s.recorder->stops, 1);
    EXPECT_EQ(s.service->CurrentState(), RecordingState::kIdle);
}

// Stop while idle is a harmless no-op (idempotent cleanup path).
TEST(ContinuousRecorderTest, StopWhenIdleIsNoop) {
    Svc s;
    auto result = s.service->Stop();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(s.recorder->stops, 0);
}

// Disk-full blocks START with a defined failure.
TEST(ContinuousRecorderTest, DiskFullBlocksStart) {
    Svc s;
    s.guard.has_space = false;
    // Rebuild service so it captures the disk-full guard.
    auto r = std::make_unique<FakeRecorder>();
    auto t = std::make_unique<FakeThumbnailWriter>();
    auto* rec = r.get();
    RecordingService svc(std::move(r), std::move(t), s.guard);
    EXPECT_FALSE(svc.StartRecording(kVideo, kThumb));
    EXPECT_EQ(rec->starts, 0);
    EXPECT_EQ(svc.CurrentState(), RecordingState::kIdle);
}

// Frames are only pushed to the recorder while recording.
TEST(ContinuousRecorderTest, FramesPushedOnlyWhileActive) {
    Svc s;
    s.service->Push(MakeFrame());  // idle -> ignored
    EXPECT_EQ(s.recorder->pushes, 0);
    ASSERT_TRUE(s.service->StartRecording(kVideo, kThumb));
    s.service->Push(MakeFrame());
    EXPECT_EQ(s.recorder->pushes, 1);
}

}  // namespace
