#include "adapters/storage/opencv/opencv-jpeg-encoder.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace sst::adapters::storage {

namespace {
using sst::common::PixelFormat;

// Wrap the frame's first plane in a BGR cv::Mat, converting color order. Returns
// an empty Mat for unsupported formats / missing data.
auto ToBgr(const sst::capture::Frame& frame) -> cv::Mat {
    if (frame.planes.empty() || frame.planes[0].data == nullptr) {
        return {};
    }
    const int width = static_cast<int>(frame.geometry.width);
    const int height = static_cast<int>(frame.geometry.height);
    if (width <= 0 || height <= 0) {
        return {};
    }

    const auto& plane = frame.planes[0];
    auto* data = const_cast<std::uint8_t*>(plane.data);  // imencode does not modify
    const auto step = static_cast<std::size_t>(plane.stride);

    cv::Mat bgr;
    switch (frame.format) {
        case PixelFormat::BGR8:
            // Clone so the returned Mat owns its bytes independently of `frame`.
            bgr = cv::Mat(height, width, CV_8UC3, data, step).clone();
            break;
        case PixelFormat::RGB8:
            cv::cvtColor(cv::Mat(height, width, CV_8UC3, data, step), bgr, cv::COLOR_RGB2BGR);
            break;
        case PixelFormat::BGRA8:
            cv::cvtColor(cv::Mat(height, width, CV_8UC4, data, step), bgr, cv::COLOR_BGRA2BGR);
            break;
        case PixelFormat::RGBA8:
            cv::cvtColor(cv::Mat(height, width, CV_8UC4, data, step), bgr, cv::COLOR_RGBA2BGR);
            break;
        case PixelFormat::GRAY8:
            cv::cvtColor(cv::Mat(height, width, CV_8UC1, data, step), bgr, cv::COLOR_GRAY2BGR);
            break;
        default:
            spdlog::warn("OpenCvJpegEncoder: unsupported format {} for JPEG",
                         sst::common::ToString(frame.format));
            return {};
    }
    return bgr;
}

}  // namespace

auto OpenCvJpegEncoder::Encode(const sst::capture::Frame& frame, sst::common::OutputSize size,
                               std::uint32_t quality)
    -> std::optional<std::vector<std::uint8_t>> {
    cv::Mat bgr = ToBgr(frame);
    if (bgr.empty()) {
        return std::nullopt;
    }

    // Optional resize to the requested output size (both dimensions must be set
    // to take effect; OutputSize{0, 0} means "keep source size").
    if (size.width > 0 && size.height > 0 &&
        (static_cast<int>(size.width) != bgr.cols || static_cast<int>(size.height) != bgr.rows)) {
        cv::Mat resized;
        cv::resize(bgr, resized,
                   cv::Size(static_cast<int>(size.width), static_cast<int>(size.height)), 0, 0,
                   cv::INTER_AREA);
        bgr = std::move(resized);
    }

    std::vector<int> params;
    if (quality > 0) {
        constexpr std::uint32_t kMaxQuality = 100;
        params.push_back(cv::IMWRITE_JPEG_QUALITY);
        params.push_back(static_cast<int>(std::min(quality, kMaxQuality)));
    }

    std::vector<std::uint8_t> out;
    if (!cv::imencode(".jpg", bgr, out, params) || out.empty()) {
        spdlog::error("OpenCvJpegEncoder: cv::imencode failed");
        return std::nullopt;
    }
    return out;
}

}  // namespace sst::adapters::storage
