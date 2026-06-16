#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "domain/buffer/services/materialize-frame.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace sst::buffer {

namespace {

// Strong-typed bundles so call sites can't transpose dimensions/components.
struct Dimensions {
    std::uint32_t width;
    std::uint32_t height;
};

struct Bgr {
    std::uint8_t blue;
    std::uint8_t green;
    std::uint8_t red;
};

struct Yuv {
    std::uint8_t luma;
    std::uint8_t chroma_u;
    std::uint8_t chroma_v;
};

constexpr std::size_t kBgrChannels = 3;

// Builds a 1-plane BGR8 Frame whose owner pins a vector<uint8_t>.
auto MakeBgr8Frame(Dimensions dims, Bgr color) -> sst::capture::Frame {
    constexpr std::uint64_t kFrameId = 42;
    constexpr std::chrono::milliseconds kCaptureMs{12345};
    auto buf = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(dims.width) *
                                                           dims.height * kBgrChannels);
    for (std::size_t i = 0; i < buf->size(); i += kBgrChannels) {
        (*buf)[i + 0] = color.blue;
        (*buf)[i + 1] = color.green;
        (*buf)[i + 2] = color.red;
    }
    sst::capture::Frame frame;
    frame.frame_id = kFrameId;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {dims.width, dims.height};
    frame.captured_at = sst::common::Timestamp{kCaptureMs};
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = dims.width * kBgrChannels,
        .data = buf->data(),
        .size = buf->size(),
    });
    frame.owner = std::shared_ptr<void>(buf);
    return frame;
}

// Builds a 2-plane NV12 Frame with optional row-stride padding (>= width).
// Y plane filled with `color.luma`, UV plane filled with alternating u, v.
auto MakeNv12Frame(Dimensions dims, Yuv color, std::uint32_t stride = 0) -> sst::capture::Frame {
    constexpr std::uint64_t kFrameId = 7;
    constexpr std::chrono::milliseconds kCaptureMs{99};
    if (stride == 0) {
        stride = dims.width;
    }
    const std::size_t y_size = static_cast<std::size_t>(stride) * dims.height;
    const std::size_t uv_size = static_cast<std::size_t>(stride) * (dims.height / 2);
    auto buf = std::make_shared<std::vector<std::uint8_t>>(y_size + uv_size);

    // Fill Y plane (only the first `width` bytes of each row are meaningful).
    for (std::uint32_t row = 0; row < dims.height; ++row) {
        for (std::uint32_t col = 0; col < dims.width; ++col) {
            (*buf)[(static_cast<std::size_t>(row) * stride) + col] = color.luma;
        }
    }
    // Fill UV plane (interleaved, half-height).
    for (std::uint32_t row = 0; row < dims.height / 2; ++row) {
        for (std::uint32_t col = 0; col < dims.width; col += 2) {
            (*buf)[y_size + (static_cast<std::size_t>(row) * stride) + col + 0] = color.chroma_u;
            (*buf)[y_size + (static_cast<std::size_t>(row) * stride) + col + 1] = color.chroma_v;
        }
    }

    sst::capture::Frame frame;
    frame.frame_id = kFrameId;
    frame.format = sst::common::PixelFormat::NV12;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {dims.width, dims.height};
    frame.captured_at = sst::common::Timestamp{kCaptureMs};
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = stride,
        .data = buf->data(),
        .size = y_size,
    });
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = stride,
        .data = buf->data() + y_size,
        .size = uv_size,
    });
    frame.owner = std::shared_ptr<void>(buf);
    return frame;
}

// Asserts every BGR triple in `plane` matches `color`.
void ExpectBgrFill(const sst::capture::FramePlane& plane, Bgr color) {
    for (std::size_t i = 0; i < plane.size; i += kBgrChannels) {
        EXPECT_EQ(plane.data[i + 0], color.blue);
        EXPECT_EQ(plane.data[i + 1], color.green);
        EXPECT_EQ(plane.data[i + 2], color.red);
    }
}

// Asserts the meaningful `dims` region of a strided Y plane equals `luma`.
void ExpectYPlaneFill(const sst::capture::FramePlane& plane, Dimensions dims, std::uint8_t luma) {
    for (std::uint32_t row = 0; row < dims.height; ++row) {
        for (std::uint32_t col = 0; col < dims.width; ++col) {
            EXPECT_EQ(plane.data[(static_cast<std::size_t>(row) * plane.stride) + col], luma);
        }
    }
}

}  // namespace

TEST(MaterializeFrameTest, RoundTripPixelsNv12) {
    // Self-evident small NV12 test geometry and fill values.
    auto src = MakeNv12Frame(
        {.width = 64, .height = 32},                       // NOLINT(readability-magic-numbers)
        {.luma = 180, .chroma_u = 100, .chroma_v = 200});  // NOLINT(readability-magic-numbers)

    auto out = MaterializeFrame(src);

    ASSERT_EQ(out.planes.size(), 2U);
    ASSERT_EQ(out.planes[0].size, src.planes[0].size);
    ASSERT_EQ(out.planes[1].size, src.planes[1].size);
    EXPECT_EQ(0, std::memcmp(out.planes[0].data, src.planes[0].data, src.planes[0].size));
    EXPECT_EQ(0, std::memcmp(out.planes[1].data, src.planes[1].data, src.planes[1].size));
}

TEST(MaterializeFrameTest, RoundTripPixelsBgr8) {
    // Self-evident small BGR8 test geometry and fill values.
    auto src =
        MakeBgr8Frame({.width = 32, .height = 32},            // NOLINT(readability-magic-numbers)
                      {.blue = 10, .green = 20, .red = 30});  // NOLINT(readability-magic-numbers)

    auto out = MaterializeFrame(src);

    ASSERT_EQ(out.planes.size(), 1U);
    ASSERT_EQ(out.planes[0].size, src.planes[0].size);
    EXPECT_EQ(0, std::memcmp(out.planes[0].data, src.planes[0].data, src.planes[0].size));
}

TEST(MaterializeFrameTest, OutputOwnsMemoryReleasesInputOwner) {
    auto out = [] {
        auto src = MakeBgr8Frame({.width = 16, .height = 16},  // NOLINT(readability-magic-numbers)
                                 {.blue = 1, .green = 2, .red = 3});
        return MaterializeFrame(src);
    }();  // src and its vector are dropped here.

    ASSERT_EQ(out.planes.size(), 1U);
    ASSERT_NE(out.owner, nullptr);
    // Reading every byte must not crash and must match the fill.
    ExpectBgrFill(out.planes[0], {.blue = 1, .green = 2, .red = 3});
}

TEST(MaterializeFrameTest, MultiPlaneStridesPreserved) {
    constexpr std::uint32_t kWidth = 32;
    constexpr std::uint32_t kHeight = 16;
    constexpr std::uint32_t kStridePad = 16;
    constexpr std::uint32_t kStride = kWidth + kStridePad;
    constexpr std::uint8_t kLuma = 50;
    constexpr std::uint8_t kChromaU = 60;
    constexpr std::uint8_t kChromaV = 70;
    auto src = MakeNv12Frame({.width = kWidth, .height = kHeight},
                             {.luma = kLuma, .chroma_u = kChromaU, .chroma_v = kChromaV}, kStride);

    auto out = MaterializeFrame(src);

    ASSERT_EQ(out.planes.size(), 2U);
    EXPECT_EQ(out.planes[0].stride, kStride);
    EXPECT_EQ(out.planes[1].stride, kStride);
    EXPECT_EQ(out.geometry.width, kWidth);
    EXPECT_EQ(out.geometry.height, kHeight);

    // Pixel at (row, col) in Y plane must match input.
    ExpectYPlaneFill(out.planes[0], {.width = kWidth, .height = kHeight}, kLuma);
}

TEST(MaterializeFrameTest, MetadataPropagated) {
    constexpr std::uint64_t kFrameId = 0xDEADBEEF;
    constexpr std::chrono::milliseconds kCaptureMs{555};
    // 8x8 is a self-evident minimal test frame.
    auto src = MakeBgr8Frame({.width = 8, .height = 8},  // NOLINT(readability-magic-numbers)
                             {.blue = 0, .green = 0, .red = 0});
    src.frame_id = kFrameId;
    src.captured_at = sst::common::Timestamp{kCaptureMs};

    auto out = MaterializeFrame(src);

    EXPECT_EQ(out.frame_id, kFrameId);
    EXPECT_EQ(out.format, sst::common::PixelFormat::BGR8);
    EXPECT_EQ(out.memory, sst::common::MemoryType::CPU);
    EXPECT_EQ(out.geometry.width, 8U);
    EXPECT_EQ(out.geometry.height, 8U);
    EXPECT_EQ(out.captured_at, src.captured_at);
}

TEST(MaterializeFrameTest, OriginalOwnerNotMutated) {
    // 8x8 is a self-evident minimal test frame.
    auto src = MakeBgr8Frame({.width = 8, .height = 8},  // NOLINT(readability-magic-numbers)
                             {.blue = 0, .green = 0, .red = 0});
    const auto before = src.owner.use_count();

    auto out = MaterializeFrame(src);

    EXPECT_EQ(src.owner.use_count(), before);
    EXPECT_NE(out.owner, src.owner);
}

}  // namespace sst::buffer
