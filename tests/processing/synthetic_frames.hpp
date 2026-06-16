#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/common/models/timestamp.hpp"

namespace sst::tests::processing {

constexpr std::size_t kBgrChannels = 3;
constexpr std::chrono::milliseconds kSyntheticCaptureTime{1000};

// Builds a 2-plane NV12 Frame whose `owner` pins a vector<uint8_t>.
// Y plane filled with `luma`, UV plane filled with alternating (chroma_u,
// chroma_v) pairs. `stride == 0` means stride = width (no row padding).
// The fill bytes are convertible same-typed args, but this is a test data
// builder — callers pass them explicitly, so the swap risk is visible in the
// test and a grouping struct would only ripple across call sites.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
inline auto MakeNv12Frame(std::uint32_t width, std::uint32_t height, std::uint8_t luma,
                          std::uint8_t chroma_u, std::uint8_t chroma_v, std::uint32_t stride = 0,
                          std::uint64_t frame_id = 1) -> sst::capture::Frame {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (stride == 0) {
        stride = width;
    }
    const std::size_t y_size = static_cast<std::size_t>(stride) * height;
    const std::size_t uv_size = static_cast<std::size_t>(stride) * (height / 2);
    auto buf = std::make_shared<std::vector<std::uint8_t>>(y_size + uv_size);

    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t col = 0; col < width; ++col) {
            (*buf)[static_cast<std::size_t>(row) * stride + col] = luma;
        }
    }
    for (std::uint32_t row = 0; row < height / 2; ++row) {
        for (std::uint32_t col = 0; col < width; col += 2) {
            (*buf)[y_size + static_cast<std::size_t>(row) * stride + col + 0] = chroma_u;
            (*buf)[y_size + static_cast<std::size_t>(row) * stride + col + 1] = chroma_v;
        }
    }

    sst::capture::Frame frame;
    frame.frame_id = frame_id;
    frame.format = sst::common::PixelFormat::NV12;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {width, height};
    frame.captured_at = sst::common::Timestamp{kSyntheticCaptureTime};
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

// Builds a 1-plane BGR8 Frame (stride == width*3, no padding).
// NOLINTBEGIN(bugprone-easily-swappable-parameters) — test data builder; the
// blue/green/red bytes are passed explicitly by name at every call site.
inline auto MakeBgr8Frame(std::uint32_t width, std::uint32_t height, std::uint8_t blue,
                          std::uint8_t green, std::uint8_t red) -> sst::capture::Frame {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto buf = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(width) * height *
                                                           kBgrChannels);
    for (std::size_t i = 0; i < buf->size(); i += kBgrChannels) {
        (*buf)[i + 0] = blue;
        (*buf)[i + 1] = green;
        (*buf)[i + 2] = red;
    }
    sst::capture::Frame frame;
    frame.frame_id = 1;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.memory = sst::common::MemoryType::CPU;
    frame.geometry = {width, height};
    frame.captured_at = sst::common::Timestamp{kSyntheticCaptureTime};
    frame.planes.push_back(sst::capture::FramePlane{
        .stride = static_cast<std::uint32_t>(width * kBgrChannels),
        .data = buf->data(),
        .size = buf->size(),
    });
    frame.owner = std::shared_ptr<void>(buf);
    return frame;
}

}  // namespace sst::tests::processing
