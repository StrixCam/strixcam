// OpenCV JPEG thumbnail writer (U10). Runs in-container (CPU OpenCV).

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "adapters/storage/opencv/opencv-thumbnail-writer.hpp"
#include "domain/capture/models/frame.hpp"

namespace fs = std::filesystem;

namespace {

auto TempFile(const std::string& suffix) -> fs::path {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("sst_thumb_" + std::to_string(stamp) + "_" +
                                        std::to_string(counter.fetch_add(1)) + suffix);
}

// A BGR frame is encoded to a non-empty JPEG on disk.
TEST(ThumbnailWriterTest, WritesBgrFrameToJpeg) {
    constexpr int kWidth = 16;
    constexpr int kHeight = 16;
    constexpr int kBgrChannels = 3;
    constexpr std::uint8_t kFill = 120;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * kBgrChannels,
                                     kFill);

    sst::capture::Frame frame;
    frame.geometry = {.width = kWidth, .height = kHeight};
    frame.format = sst::common::PixelFormat::BGR8;
    frame.planes.push_back(
        {.stride = kWidth * kBgrChannels, .data = pixels.data(), .size = pixels.size()});

    const fs::path out = TempFile(".jpg");
    sst::adapters::storage::OpenCvThumbnailWriter writer;
    ASSERT_TRUE(writer.Write(frame, out));
    ASSERT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0U);
    fs::remove(out);
}

// A frame with no pixel data fails cleanly.
TEST(ThumbnailWriterTest, EmptyFrameFails) {
    constexpr std::uint32_t kDim = 16;
    sst::capture::Frame frame;
    frame.geometry = {.width = kDim, .height = kDim};
    frame.format = sst::common::PixelFormat::BGR8;
    sst::adapters::storage::OpenCvThumbnailWriter writer;
    EXPECT_FALSE(writer.Write(frame, TempFile(".jpg")));
}

}  // namespace
