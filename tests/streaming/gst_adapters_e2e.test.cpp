// End-to-end exercise of the streaming GStreamer adapters.
//
// Hardware-bound: needs NVENC (`nvv4l2h264enc` + `nvvidconv`) on a Jetson and,
// for the RTMP test, network egress to an RTMP ingest endpoint. In the
// cross-compile dev container these tests fail at pipeline launch — that's
// expected. They pass on-device.

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "adapters/streaming/gst_rtmp/gst-rtmp-streamer.hpp"
#include "adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/streaming/models/app-stream-config.hpp"
#include "domain/streaming/models/platform-stream-config.hpp"

namespace {

using sst::adapters::streaming::GstRtmpStreamer;
using sst::adapters::streaming::GstRtspAppStreamServer;

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFps = 30;
constexpr int kBgrChannels = 3;
constexpr int kRtspTestPort = 18554;  // out of the way of any production server
constexpr int kRtspBitrateKbps = 1500;
constexpr int kRtmpBitrateKbps = 2000;
constexpr std::int64_t kLocalIngestStreamId = 99;

auto NextSuffix() -> std::string {
    static std::atomic<int> counter{0};
    return std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
}

// frame_id and value are distinct concepts with distinct widths; the helper is
// only called below with clearly-named arguments, so the convertible-type swap
// warning is a false positive here.
auto MakeBgr8Frame(
    std::uint64_t frame_id,  // NOLINT(bugprone-easily-swappable-parameters) floor-ok: test data
                             // builder; distinct widths, named call sites
    std::uint8_t value, std::shared_ptr<std::vector<std::uint8_t>>& storage)
    -> sst::capture::Frame {
    const std::size_t row_bytes = static_cast<std::size_t>(kWidth) * kBgrChannels;
    const std::size_t total = row_bytes * static_cast<std::size_t>(kHeight);
    storage = std::make_shared<std::vector<std::uint8_t>>(total, value);
    sst::capture::Frame frame;
    frame.frame_id = frame_id;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {.width = kWidth, .height = kHeight};
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = static_cast<std::uint32_t>(row_bytes),
        .data = storage->data(),
        .size = total,
    });
    frame.captured_at = std::chrono::steady_clock::now();
    frame.owner = storage;
    return frame;
}

template <typename Sink>
auto PumpFrames(Sink& sink, int frame_count) -> void {
    const auto interval = std::chrono::microseconds(1'000'000 / kFps);
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < frame_count; ++i) {
        std::shared_ptr<std::vector<std::uint8_t>> storage;
        const auto value = static_cast<std::uint8_t>(i & 0xFF);
        auto frame = MakeBgr8Frame(static_cast<std::uint64_t>(i), value, storage);
        sink.Push(frame);
        next += interval;
        std::this_thread::sleep_until(next);
    }
}

// ── RTSP app-stream server ────────────────────────────────────────────────

TEST(StreamingE2E, AppStreamServerStartsAndAcceptsPushes) {
    GstRtspAppStreamServer server;

    sst::streaming::AppStreamConfig cfg;
    cfg.mount_point = "/test_" + NextSuffix();
    cfg.port = kRtspTestPort;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.framerate = kFps;
    cfg.bitrate_kbps = kRtspBitrateKbps;

    ASSERT_TRUE(server.Start(cfg));
    EXPECT_TRUE(server.IsRunning());

    // Without a real RTSP client there's no appsrc bound yet; Push() is still
    // safe and silently drops. Once a phone connects to rtsp://<ip>:18554<mount>
    // the server's media-configure callback wires the appsrc and Push() routes
    // frames into it.
    PumpFrames(server, kFps);
    EXPECT_TRUE(server.IsRunning());

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

// ── RTMP streamer ─────────────────────────────────────────────────────────

TEST(StreamingE2E, RtmpStreamerRejectsEmptyUrl) {
    GstRtmpStreamer streamer;
    sst::streaming::PlatformStreamConfig cfg;
    cfg.stream_id = 1;
    cfg.name = "no-url";
    cfg.url = "";
    cfg.stream_key = "key";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.framerate = kFps;
    cfg.bitrate_kbps = kRtmpBitrateKbps;
    EXPECT_FALSE(streamer.Start(cfg));
    EXPECT_FALSE(streamer.IsRunning());
}

TEST(StreamingE2E, RtmpStreamerRejectsEmptyKey) {
    GstRtmpStreamer streamer;
    sst::streaming::PlatformStreamConfig cfg;
    cfg.stream_id = 2;
    cfg.name = "no-key";
    cfg.url = "rtmp://localhost:1935/live";
    cfg.stream_key = "";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.framerate = kFps;
    cfg.bitrate_kbps = kRtmpBitrateKbps;
    EXPECT_FALSE(streamer.Start(cfg));
    EXPECT_FALSE(streamer.IsRunning());
}

TEST(StreamingE2E, RtmpStreamerStartsAndPushesAgainstLocalIngest) {
    // Requires an RTMP listener on localhost:1935/live (e.g. nginx-rtmp).
    // On-device CI should provision one; in the dev container this fails at
    // PLAYING transition because `rtmpsink` can't connect.
    GstRtmpStreamer streamer;
    sst::streaming::PlatformStreamConfig cfg;
    cfg.stream_id = kLocalIngestStreamId;
    cfg.name = "local-test";
    cfg.url = "rtmp://localhost:1935/live";
    cfg.stream_key = "test-" + NextSuffix();
    cfg.type = sst::streaming::PlatformStreamType::kRtmp;
    cfg.codec = sst::streaming::PlatformStreamCodec::kH264;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.framerate = kFps;
    cfg.bitrate_kbps = kRtmpBitrateKbps;

    ASSERT_TRUE(streamer.Start(cfg));
    EXPECT_TRUE(streamer.IsRunning());
    EXPECT_EQ(streamer.Id(), kLocalIngestStreamId);

    PumpFrames(streamer, kFps * 2);  // 2 seconds of frames

    streamer.Stop();
    EXPECT_FALSE(streamer.IsRunning());
}

}  // namespace
