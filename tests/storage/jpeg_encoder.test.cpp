// OpenCV in-memory JPEG encoder (U9). Runs in-container (CPU OpenCV). Encodes a
// frame to JPEG bytes for the on-demand BLE ThumbnailRequest path.

#include <gtest/gtest.h>

#include <cstdint>
#include <opencv2/imgcodecs.hpp>
#include <vector>

#include "adapters/storage/opencv/opencv-jpeg-encoder.hpp"
#include "domain/capture/models/frame.hpp"

namespace {

using sst::adapters::storage::OpenCvJpegEncoder;

constexpr int kBgrChannels = 3;

auto MakeBgrFrame(int width, int height,
                  std::uint8_t value) -> std::pair<sst::capture::Frame, std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * kBgrChannels,
                                     value);
    sst::capture::Frame frame;
    frame.geometry = {.width = static_cast<std::uint32_t>(width),
                      .height = static_cast<std::uint32_t>(height)};
    frame.format = sst::common::PixelFormat::BGR8;
    frame.planes.push_back({.stride = static_cast<std::uint32_t>(width * kBgrChannels),
                            .data = pixels.data(),
                            .size = pixels.size()});
    return {frame, std::move(pixels)};
}

// A BGR frame encodes to a JPEG that decodes back at the source dimensions.
TEST(JpegEncoderTest, EncodesBgrFrameToJpegBytes) {
    constexpr int kSrcWidth = 32;
    constexpr int kSrcHeight = 24;
    constexpr std::uint8_t kFill = 120;
    auto [frame, storage] = MakeBgrFrame(kSrcWidth, kSrcHeight, kFill);
    OpenCvJpegEncoder encoder;
    auto bytes = encoder.Encode(frame, sst::common::OutputSize{0, 0}, /*quality=*/0);
    ASSERT_TRUE(bytes.has_value());
    ASSERT_GE(bytes->size(), 2U);
    EXPECT_EQ((*bytes)[0], 0xFF);  // JPEG Start Of Image marker
    EXPECT_EQ((*bytes)[1], 0xD8);

    // Decode the output and assert it is a real JPEG of the source size.
    const cv::Mat decoded = cv::imdecode(cv::Mat(*bytes), cv::IMREAD_COLOR);
    ASSERT_FALSE(decoded.empty()) << "encoded bytes did not decode as a JPEG";
    EXPECT_EQ(decoded.cols, kSrcWidth);
    EXPECT_EQ(decoded.rows, kSrcHeight);
}

// A requested output size resizes before encoding — the decoded JPEG carries the
// requested dimensions, not the source's.
TEST(JpegEncoderTest, ResizesToRequestedDimensions) {
    constexpr int kSrcDim = 64;
    constexpr std::uint8_t kFill = 200;
    constexpr std::uint32_t kOutDim = 16;
    constexpr std::uint32_t kQuality = 80;
    auto [frame, storage] = MakeBgrFrame(kSrcDim, kSrcDim, kFill);
    OpenCvJpegEncoder encoder;
    auto small = encoder.Encode(frame, sst::common::OutputSize{kOutDim, kOutDim}, kQuality);
    ASSERT_TRUE(small.has_value());
    EXPECT_FALSE(small->empty());

    const cv::Mat decoded = cv::imdecode(cv::Mat(*small), cv::IMREAD_COLOR);
    ASSERT_FALSE(decoded.empty()) << "resized output did not decode as a JPEG";
    EXPECT_EQ(decoded.cols, static_cast<int>(kOutDim));
    EXPECT_EQ(decoded.rows, static_cast<int>(kOutDim));
}

// A frame with no pixel data fails cleanly (nullopt, no crash).
TEST(JpegEncoderTest, EmptyFrameReturnsNullopt) {
    constexpr std::uint32_t kDim = 16;
    sst::capture::Frame frame;
    frame.geometry = {.width = kDim, .height = kDim};
    frame.format = sst::common::PixelFormat::BGR8;
    OpenCvJpegEncoder encoder;
    EXPECT_FALSE(encoder.Encode(frame, sst::common::OutputSize{0, 0}, 0).has_value());
}

}  // namespace
