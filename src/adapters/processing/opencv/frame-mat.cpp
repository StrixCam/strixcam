#include "adapters/processing/opencv/frame-mat.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <vector>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/common/models/timestamp.hpp"

namespace sst::adapters::processing {

auto WrapNv12(const sst::capture::Frame& frame) -> std::optional<cv::Mat> {
    if (frame.format != sst::common::PixelFormat::NV12) {
        return std::nullopt;
    }
    if (frame.planes.size() != 2) {
        return std::nullopt;
    }
    const auto width = frame.geometry.width;
    const auto height = frame.geometry.height;
    if (width == 0 || height == 0 || (width % 2) != 0 || (height % 2) != 0) {
        return std::nullopt;
    }

    const auto& y_plane = frame.planes[0];
    const auto& uv_plane = frame.planes[1];
    if (y_plane.data == nullptr || uv_plane.data == nullptr) {
        return std::nullopt;
    }

    const bool contiguous = (y_plane.stride == width) && (uv_plane.stride == width) &&
                            (y_plane.data + y_plane.size == uv_plane.data);

    const int total_rows = static_cast<int>(height) + static_cast<int>(height) / 2;

    if (contiguous) {
        // Non-owning Mat over the input memory.
        return cv::Mat(total_rows, static_cast<int>(width), CV_8UC1,
                       const_cast<std::uint8_t*>(y_plane.data), y_plane.stride);
    }

    // Fallback: allocate compact (H*3/2 x W) CV_8UC1 and row-copy.
    cv::Mat out(total_rows, static_cast<int>(width), CV_8UC1);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(out.ptr(static_cast<int>(row)),
                    y_plane.data + static_cast<std::size_t>(row) * y_plane.stride, width);
    }
    for (std::uint32_t row = 0; row < height / 2; ++row) {
        std::memcpy(out.ptr(static_cast<int>(height + row)),
                    uv_plane.data + static_cast<std::size_t>(row) * uv_plane.stride, width);
    }
    return out;
}

auto MakeOwnedFrame(const cv::Mat& mat, sst::common::PixelFormat fmt, std::uint64_t frame_id,
                    sst::common::Timestamp captured_at) -> sst::capture::Frame {
    assert(mat.depth() == CV_8U && "MakeOwnedFrame requires CV_8U mat");
    assert(mat.cols > 0 && mat.rows > 0);

    const auto cols = static_cast<std::uint32_t>(mat.cols);
    const auto rows = static_cast<std::uint32_t>(mat.rows);
    const auto channels = static_cast<std::uint32_t>(mat.channels());
    const std::size_t row_bytes = static_cast<std::size_t>(cols) * channels;
    const std::size_t total = static_cast<std::size_t>(rows) * row_bytes;

    auto buf = std::make_shared<std::vector<std::uint8_t>>(total);
    if (mat.isContinuous()) {
        // Hot path: every fresh cvtColor/resize output is continuous — one bulk
        // copy instead of a per-row loop.
        std::memcpy(buf->data(), mat.ptr(0), total);
    } else {
        for (std::uint32_t row = 0; row < rows; ++row) {
            std::memcpy(buf->data() + static_cast<std::size_t>(row) * row_bytes,
                        mat.ptr(static_cast<int>(row)), row_bytes);
        }
    }

    sst::capture::Frame out;
    out.frame_id = frame_id;
    out.format = fmt;
    out.memory = sst::common::MemoryType::CPU;
    out.geometry = {cols, rows};
    out.captured_at = captured_at;
    out.planes.push_back(sst::capture::FramePlane{
        .stride = static_cast<std::uint32_t>(row_bytes),
        .data = buf->data(),
        .size = total,
    });
    out.owner = std::shared_ptr<void>(buf);
    return out;
}

}  // namespace sst::adapters::processing
