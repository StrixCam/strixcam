#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/streaming/ports/app-stream-server.hpp"
#include "app/streaming/ports/platform-streamer.hpp"
#include "app/streaming/services/streaming_service/streaming-service.hpp"
#include "domain/streaming/models/app-stream-config.hpp"
#include "domain/streaming/models/platform-stream-config.hpp"

namespace {

using sst::streaming::ActivePlatformStream;
using sst::streaming::AppStreamConfig;
using sst::streaming::IAppStreamServer;
using sst::streaming::IPlatformStreamer;
using sst::streaming::PlatformStreamConfig;
using sst::streaming::StreamingService;

// ── Test doubles ──────────────────────────────────────────────────────────
//
// Stand in for the GStreamer adapters (which need NVENC and aren't available
// in the cross-compile container). Each captures method calls so the service
// assertions can verify interactions.

class FakeAppStreamServer final : public IAppStreamServer {
   public:
    auto Start(const AppStreamConfig& config) -> bool override {
        ++start_calls;
        last_config = config;
        running_ = accept_start;
        return running_;
    }

    auto Stop() -> void override {
        ++stop_calls;
        running_ = false;
    }

    [[nodiscard]] auto IsRunning() const -> bool override { return running_; }

    auto Push(const sst::capture::Frame& /*frame*/) -> void override { ++push_calls; }

    int start_calls{0};
    int stop_calls{0};
    int push_calls{0};
    bool accept_start{true};
    AppStreamConfig last_config{};

   private:
    bool running_{false};
};

// Counters live outside the streamer so they survive the streamer's
// destruction (the service drops the streamer when StopPlatformStream is
// called). Tests inspect this struct after the streamer dies.
struct PlatformStreamerCounters {
    int start_calls{0};
    int stop_calls{0};
    int push_calls{0};
    bool accept_start{true};
};

class FakePlatformStreamer final : public IPlatformStreamer {
   public:
    explicit FakePlatformStreamer(PlatformStreamerCounters* counters) : counters_(counters) {}

    auto Start(const PlatformStreamConfig& config) -> bool override {
        ++counters_->start_calls;
        config_ = config;
        running_ = counters_->accept_start;
        return running_;
    }

    auto Stop() -> void override {
        ++counters_->stop_calls;
        running_ = false;
    }

    [[nodiscard]] auto IsRunning() const -> bool override { return running_; }
    [[nodiscard]] auto Id() const -> std::int64_t override { return config_.stream_id; }

    auto Push(const sst::capture::Frame& /*frame*/) -> void override { ++counters_->push_calls; }

   private:
    PlatformStreamerCounters* counters_;
    PlatformStreamConfig config_{};
    bool running_{false};
};

// Hands out a fresh PlatformStreamerCounters per mint and gives the
// FakePlatformStreamer a raw pointer to it. Counters outlive the streamer,
// which is critical because the service drops the streamer on Stop.
struct PlatformStreamerSink {
    std::vector<std::unique_ptr<PlatformStreamerCounters>> counters;

    auto MakeFactory() -> StreamingService::PlatformStreamerFactory {
        return [this] {
            auto* slot = counters.emplace_back(std::make_unique<PlatformStreamerCounters>()).get();
            return std::unique_ptr<IPlatformStreamer>(std::make_unique<FakePlatformStreamer>(slot));
        };
    }
};

// ── Fixture ───────────────────────────────────────────────────────────────

class StreamingServiceTest : public ::testing::Test {
   protected:
    auto SetUp() -> void override {
        auto app = std::make_unique<FakeAppStreamServer>();
        app_ = app.get();
        service_ = std::make_unique<StreamingService>(std::move(app), sink_.MakeFactory());
    }

    static auto MakePlatformConfig(std::int64_t id, std::string name = "test")
        -> PlatformStreamConfig {
      PlatformStreamConfig cfg;
      cfg.stream_id = id;
      cfg.name = std::move(name);
      cfg.url = "rtmp://example.com/live";
      cfg.stream_key = "abcd-1234";
      cfg.width = 1920;
      cfg.height = 1080;
      cfg.framerate = 30;
      cfg.bitrate_kbps = 4000;
      return cfg;
    }

    FakeAppStreamServer* app_{nullptr};
    PlatformStreamerSink sink_;
    std::unique_ptr<StreamingService> service_;
};

// ── App stream lifecycle ──────────────────────────────────────────────────

TEST_F(StreamingServiceTest, StartAppStreamForwardsConfigAndTracksRunning) {
    AppStreamConfig cfg;
    cfg.port = 8556;
    cfg.width = 1280;
    cfg.height = 720;

    EXPECT_FALSE(service_->IsAppStreamRunning());
    EXPECT_TRUE(service_->StartAppStream(cfg));
    EXPECT_TRUE(service_->IsAppStreamRunning());
    EXPECT_EQ(app_->start_calls, 1);
    EXPECT_EQ(app_->last_config.port, 8556);
    EXPECT_EQ(app_->last_config.width, 1280U);

    // Idempotent: start while already running succeeds without re-Start.
    EXPECT_TRUE(service_->StartAppStream(cfg));
    EXPECT_EQ(app_->start_calls, 1);
}

TEST_F(StreamingServiceTest, StartAppStreamReturnsFalseWhenAdapterRefuses) {
    app_->accept_start = false;
    EXPECT_FALSE(service_->StartAppStream({}));
    EXPECT_FALSE(service_->IsAppStreamRunning());
    EXPECT_EQ(app_->start_calls, 1);
}

TEST_F(StreamingServiceTest, StopAppStream) {
    ASSERT_TRUE(service_->StartAppStream({}));
    EXPECT_TRUE(service_->StopAppStream());
    EXPECT_FALSE(service_->IsAppStreamRunning());
    EXPECT_EQ(app_->stop_calls, 1);

    // Stopping again returns false (precondition: must be running).
    EXPECT_FALSE(service_->StopAppStream());
}

// ── Platform streams ──────────────────────────────────────────────────────

TEST_F(StreamingServiceTest, StartPlatformStreamMintsStreamerAndTracksId) {
    auto cfg = MakePlatformConfig(42, "youtube-main");
    EXPECT_TRUE(service_->StartPlatformStream(cfg));

    ASSERT_EQ(sink_.counters.size(), 1);
    EXPECT_EQ(sink_.counters[0]->start_calls, 1);

    auto active = service_->ListActivePlatformStreams();
    ASSERT_EQ(active.size(), 1);
    EXPECT_EQ(active[0].stream_id, 42);
    EXPECT_EQ(active[0].name, "youtube-main");
}

TEST_F(StreamingServiceTest, StartPlatformStreamRejectsInvalidId) {
    auto cfg = MakePlatformConfig(0);
    EXPECT_FALSE(service_->StartPlatformStream(cfg));
    EXPECT_TRUE(sink_.counters.empty());
    EXPECT_TRUE(service_->ListActivePlatformStreams().empty());
}

TEST_F(StreamingServiceTest, StartPlatformStreamRejectsDuplicateId) {
    auto cfg = MakePlatformConfig(7);
    ASSERT_TRUE(service_->StartPlatformStream(cfg));

    EXPECT_FALSE(service_->StartPlatformStream(cfg));
    // A second factory invocation must not have happened.
    EXPECT_EQ(sink_.counters.size(), 1);
}

TEST_F(StreamingServiceTest, StopPlatformStreamRemovesFromActiveAndStops) {
    auto cfg = MakePlatformConfig(3);
    ASSERT_TRUE(service_->StartPlatformStream(cfg));
    auto* counters = sink_.counters[0].get();

    EXPECT_TRUE(service_->StopPlatformStream(3));
    EXPECT_EQ(counters->stop_calls, 1);
    EXPECT_TRUE(service_->ListActivePlatformStreams().empty());

    EXPECT_FALSE(service_->StopPlatformStream(3));  // no longer present
}

TEST_F(StreamingServiceTest, ListActiveReflectsEachStartAndStop) {
    ASSERT_TRUE(service_->StartPlatformStream(MakePlatformConfig(1, "yt")));
    ASSERT_TRUE(service_->StartPlatformStream(MakePlatformConfig(2, "tw")));
    ASSERT_TRUE(service_->StartPlatformStream(MakePlatformConfig(3, "fb")));
    EXPECT_EQ(service_->ListActivePlatformStreams().size(), 3);

    ASSERT_TRUE(service_->StopPlatformStream(2));
    EXPECT_EQ(service_->ListActivePlatformStreams().size(), 2);
}

// ── IFrameSink fan-out ────────────────────────────────────────────────────

TEST_F(StreamingServiceTest, PushFansOutToAppStreamWhenRunning) {
    sst::capture::Frame frame;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.geometry = {.width = 1920, .height = 1080};

    service_->Push(frame);  // app stream not started yet
    EXPECT_EQ(app_->push_calls, 0);

    ASSERT_TRUE(service_->StartAppStream({}));
    service_->Push(frame);
    service_->Push(frame);
    EXPECT_EQ(app_->push_calls, 2);

    ASSERT_TRUE(service_->StopAppStream());
    service_->Push(frame);
    EXPECT_EQ(app_->push_calls, 2);
}

TEST_F(StreamingServiceTest, PushFansOutToEveryActivePlatformStream) {
    ASSERT_TRUE(service_->StartPlatformStream(MakePlatformConfig(1, "yt")));
    ASSERT_TRUE(service_->StartPlatformStream(MakePlatformConfig(2, "tw")));
    auto* yt = sink_.counters[0].get();
    auto* tw = sink_.counters[1].get();

    sst::capture::Frame frame;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.geometry = {.width = 1280, .height = 720};

    service_->Push(frame);
    service_->Push(frame);
    service_->Push(frame);

    EXPECT_EQ(yt->push_calls, 3);
    EXPECT_EQ(tw->push_calls, 3);

    ASSERT_TRUE(service_->StopPlatformStream(1));
    service_->Push(frame);
    EXPECT_EQ(yt->push_calls, 3);  // stopped, no more pushes
    EXPECT_EQ(tw->push_calls, 4);
}

}  // namespace
