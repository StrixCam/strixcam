#include "adapters/capture/frame/gstreamer/gstreamer.hpp"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "app/config/services/config_loader/config-loader.hpp"
#include "domain/capture/models/camera-config.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kConfigDir = SST_REPO_ROOT_DIR "/tests/config/config_files";

}  // namespace

// Capture test runs against real Jetson hardware (IMX477 via nvarguscamerasrc).
// Build in dev container, scp the test binary to the Jetson, run with:
//   ./sst_cam_firmware_tests --gtest_filter=GstreamerAdapter.*
TEST(GstreamerAdapter, CaptureSingleFrameAndLog) {
    sst::config::app::ConfigLoader loader(kConfigDir, "json");
    sst::config::ConfigData cfg = loader.get();
    const std::string device_model = cfg.device.model.value_or("");

    // Camera sensor config is a compiled-in default now (app-as-source-of-truth:
    // no local DB). Lens calibration still comes from calibration.json.
    const sst::capture::CameraConfig camera_cfg{};

    constexpr std::uint16_t kCameraIndex = 0;
    sst::capture::GStreamerAdapter adapter(camera_cfg, device_model, kCameraIndex);
    adapter.Start();
    ASSERT_TRUE(adapter.IsRunning()) << "GStreamer pipeline did not start";

    std::optional<sst::capture::Frame> frame;
    std::optional<sst::capture::Frame> prev;

    constexpr int kMaxSeconds = 5;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxSeconds);

    spdlog::info("Starting frame capture loop...");

    while (std::chrono::steady_clock::now() < deadline) {
        auto captured = adapter.Capture();
        if (!captured) {
            std::this_thread::yield();
            continue;
        }

        frame = *captured;

        if (prev) {
            auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                frame->captured_at - prev->captured_at)
                                .count();
            if (delta_ms > 0) {
                constexpr double kMillisecondsPerSecond = 1000.0;
                double fps = kMillisecondsPerSecond / static_cast<double>(delta_ms);
                spdlog::info("frame_id={} dt={} ms (~{:.1f} fps)", frame->frame_id, delta_ms, fps);
            }
        }

        prev = *frame;
    }

    spdlog::info("Frame capture loop ended.");

    if (!frame) {
        FAIL() << "Failed to capture frame from GStreamer";
        return;
    }

    const auto ts_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(frame->captured_at.time_since_epoch())
            .count();

    spdlog::info("Captured frame:");
    spdlog::info("  frame_id      = {}", frame->frame_id);
    spdlog::info("  timestamp_ms  = {}", ts_ms);
    spdlog::info("  width         = {}", frame->geometry.width);
    spdlog::info("  height        = {}", frame->geometry.height);
    spdlog::info("  planes        = {}", frame->planes.size());
    spdlog::info("  bytes         = {}", frame->planes[0].size);
    spdlog::info("  stride        = {}", frame->planes[0].stride);
    spdlog::info("  pixel_format  = {}", static_cast<int>(frame->format));

    adapter.Stop();
    EXPECT_FALSE(adapter.IsRunning());
}
