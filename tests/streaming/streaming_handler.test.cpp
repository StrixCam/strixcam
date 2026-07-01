// Streaming control handler (U11, R22). Pure — fake IStreamingService.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app/control/services/handlers/streaming.handler.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "app/streaming/ports/uplink-probe.hpp"
#include "bluetooth.pb.h"
#include "domain/common/models/video-quality.hpp"
#include "domain/streaming/models/platform-stream-config.hpp"

namespace {

using sst::control::StreamingHandler;

class FakeUplinkProbe final : public sst::streaming::IUplinkProbe {
   public:
    auto HasInternetUplink() -> bool override { return has_uplink; }
    bool has_uplink{true};
};

class FakeStreaming final : public sst::streaming::IStreamingService {
   public:
    auto StartAppStream(const sst::streaming::AppStreamConfig& /*config*/) -> bool override {
        return true;
    }
    auto StopAppStream() -> bool override { return true; }
    [[nodiscard]] auto IsAppStreamRunning() const -> bool override { return false; }

    auto StartPlatformStream(const sst::streaming::PlatformStreamConfig& config) -> bool override {
        if (!start_ok) {
            return false;
        }
        last_url = config.url;
        last_width = config.width;
        last_height = config.height;
        last_framerate = config.framerate;
        active = true;
        return true;
    }
    auto StopPlatformStream(std::int64_t /*stream_id*/) -> bool override {
        if (!active) {
            return false;
        }
        active = false;
        return true;
    }
    [[nodiscard]] auto ListActivePlatformStreams() const
        -> std::vector<sst::streaming::ActivePlatformStream> override {
        return active ? std::vector<sst::streaming::ActivePlatformStream>{{1, "egress"}}
                      : std::vector<sst::streaming::ActivePlatformStream>{};
    }

    bool start_ok{true};
    bool active{false};
    std::string last_url;
    std::int32_t last_width{0};
    std::int32_t last_height{0};
    std::int32_t last_framerate{0};
};

auto StartCmd(const std::string& dest) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("x");
    auto* control = cmd.mutable_streaming_control();
    control->set_action(sst_cam::STREAMING_START);
    control->set_destination(dest);
    return cmd;
}

auto StopCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("x");
    cmd.mutable_streaming_control()->set_action(sst_cam::STREAMING_STOP);
    return cmd;
}

auto StartCmdWithQuality(const std::string& dest,
                         const sst::common::VideoQuality& mode) -> sst_cam::Command {
    auto cmd = StartCmd(dest);
    auto* quality = cmd.mutable_streaming_control()->mutable_quality();
    quality->set_width(static_cast<std::uint32_t>(mode.width));
    quality->set_height(static_cast<std::uint32_t>(mode.height));
    quality->set_fps(static_cast<std::uint32_t>(mode.fps));
    return cmd;
}

auto SetConfigCmd(const std::string& custom_url) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("x");
    cmd.mutable_set_streaming_config()->mutable_config()->set_custom_rtmp_url(custom_url);
    return cmd;
}

// R22: START to the supplied destination; STOP closes it; telemetry reflects it.
TEST(StreamingHandlerTest, StartToDestinationThenStop) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);

    auto start = handler.Handle(StartCmd("rtmp://ingest.example/live/key"));
    EXPECT_EQ(start.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(streaming.last_url, "rtmp://ingest.example/live/key");
    EXPECT_FALSE(streaming.ListActivePlatformStreams().empty());

    auto stop = handler.Handle(StopCmd());
    EXPECT_EQ(stop.status(), sst_cam::ResponseStatus::OK);
    EXPECT_TRUE(streaming.ListActivePlatformStreams().empty());
}

// An empty destination with no configured fallback is an error.
TEST(StreamingHandlerTest, EmptyDestinationErrors) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);
    auto resp = handler.Handle(StartCmd(""));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(resp.error_message().empty());
}

// U6 / R9: a cloud-stream START with no internet uplink is rejected with a
// clear, actionable message instead of an opaque rtmp failure.
TEST(StreamingHandlerTest, NoUplinkRejectsStartWithClearError) {
    FakeStreaming streaming;
    FakeUplinkProbe probe;
    probe.has_uplink = false;
    StreamingHandler handler(streaming, &probe);

    auto resp = handler.Handle(StartCmd("rtmp://ingest.example/live/key"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_NE(resp.error_message().find("uplink"), std::string::npos);
    EXPECT_TRUE(streaming.ListActivePlatformStreams().empty());  // never started
}

// With an uplink up, the START proceeds to the egress.
TEST(StreamingHandlerTest, UplinkUpAllowsStart) {
    FakeStreaming streaming;
    FakeUplinkProbe probe;  // has_uplink defaults true
    StreamingHandler handler(streaming, &probe);

    auto resp = handler.Handle(StartCmd("rtmp://ingest.example/live/key"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_FALSE(streaming.ListActivePlatformStreams().empty());
}

// SetStreamingConfig supplies a fallback destination used by a START with no
// explicit destination.
TEST(StreamingHandlerTest, SetConfigProvidesFallbackDestination) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);

    EXPECT_EQ(handler.Handle(SetConfigCmd("rtmp://fallback/live/abc")).status(),
              sst_cam::ResponseStatus::OK);
    auto start = handler.Handle(StartCmd(""));
    EXPECT_EQ(start.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(streaming.last_url, "rtmp://fallback/live/abc");
}

// STOP with nothing active is an error.
TEST(StreamingHandlerTest, StopWithoutActiveStreamErrors) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);
    EXPECT_EQ(handler.Handle(StopCmd()).status(), sst_cam::ResponseStatus::ERROR);
}

// U8/R15: a supported stream quality reaches the PlatformStreamConfig, independent
// of the record quality.
TEST(StreamingHandlerTest, SupportedStreamQualityAppliedToConfig) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);

    const sst::common::VideoQuality kMode{1280, 720, 30};
    auto start = handler.Handle(StartCmdWithQuality("rtmp://ingest/live/k", kMode));
    EXPECT_EQ(start.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(streaming.last_width, kMode.width);
    EXPECT_EQ(streaming.last_height, kMode.height);
    EXPECT_EQ(streaming.last_framerate, kMode.fps);
}

// No quality field → the config keeps its default resolution/fps (1080p30).
TEST(StreamingHandlerTest, MissingStreamQualityKeepsDefault) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);

    auto start = handler.Handle(StartCmd("rtmp://ingest/live/k"));
    EXPECT_EQ(start.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(streaming.last_width, sst::streaming::kDefaultWidth);
    EXPECT_EQ(streaming.last_height, sst::streaming::kDefaultHeight);
    EXPECT_EQ(streaming.last_framerate, sst::streaming::kDefaultFramerate);
    // The no-quality default MUST be a mode the hardware sustains — otherwise a
    // legitimate quality-less START runs the unsustainable software encode.
    EXPECT_TRUE(
        sst::common::IsSupportedMode({sst::streaming::kDefaultWidth, sst::streaming::kDefaultHeight,
                                      sst::streaming::kDefaultFramerate}));
}

// U8/R16: an unadvertised mode is rejected to the firmware default, never applied.
TEST(StreamingHandlerTest, UnsupportedStreamQualityKeepsDefault) {
    FakeStreaming streaming;
    StreamingHandler handler(streaming);

    const sst::common::VideoQuality kUnadvertised{640, 480, 24};
    auto start = handler.Handle(StartCmdWithQuality("rtmp://ingest/live/k", kUnadvertised));
    EXPECT_EQ(start.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(streaming.last_width, sst::streaming::kDefaultWidth);
    EXPECT_EQ(streaming.last_height, sst::streaming::kDefaultHeight);
    EXPECT_EQ(streaming.last_framerate, sst::streaming::kDefaultFramerate);
}

}  // namespace
