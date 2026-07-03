// Continuous MP4 recorder real-encode (U10, R21, AE4). Hardware-bound: needs
// NVENC (nvv4l2h264enc/nvvidconv) — expected to FAIL in the container, passes
// on-device, where it produces a single playable MP4.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "adapters/storage/gstreamer/gst-continuous-recorder.hpp"
#include "domain/capture/models/frame.hpp"

namespace fs = std::filesystem;

namespace {

TEST(GstContinuousE2E, EncodesSinglePlayableMp4) {
    const fs::path out = fs::temp_directory_path() / "sst_e2e_continuous.mp4";
    fs::remove(out);

    // Force the software scale/convert path: VIC (nvvidconv) is the on-device
    // default (U2) but does not resolve in the qemu container, so this encode/mux
    // E2E runs on the software path here. VIC is validated on-device separately.
    setenv("SST_DISABLE_VIC", "1", /*overwrite=*/1);
    sst::adapters::storage::GstContinuousRecorder recorder;
    ASSERT_TRUE(recorder.Start(out, {})) << "NVENC pipeline did not start (expected off-device)";

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kBgrChannels = 3;
    constexpr std::uint8_t kFill = 0x40;
    constexpr int kFrameCount = 30;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * kBgrChannels,
                                     kFill);
    for (int i = 0; i < kFrameCount; ++i) {
        sst::capture::Frame frame;
        frame.geometry = {.width = kWidth, .height = kHeight};
        frame.format = sst::common::PixelFormat::BGR8;
        frame.planes.push_back(
            {.stride = kWidth * kBgrChannels, .data = pixels.data(), .size = pixels.size()});
        recorder.Push(frame);
    }

    EXPECT_TRUE(recorder.Stop());
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0U);
    fs::remove(out);
}

}  // namespace
