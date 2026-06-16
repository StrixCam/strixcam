#include "adapters/storage/opencv/opencv-thumbnail-writer.hpp"

#include <spdlog/spdlog.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <system_error>

namespace sst::adapters::storage {

namespace {
using sst::common::PixelFormat;
}

auto OpenCvThumbnailWriter::Write(const sst::capture::Frame& frame,
                                  const std::filesystem::path& output_path) -> bool {
    if (frame.planes.empty() || frame.planes[0].data == nullptr) {
        spdlog::warn("OpenCvThumbnailWriter: frame has no pixel data");
        return false;
    }
    const int width = static_cast<int>(frame.geometry.width);
    const int height = static_cast<int>(frame.geometry.height);
    if (width <= 0 || height <= 0) {
        return false;
    }

    const auto& plane = frame.planes[0];
    auto* data = const_cast<std::uint8_t*>(plane.data);  // imwrite does not modify
    const auto step = static_cast<std::size_t>(plane.stride);

    cv::Mat bgr;
    switch (frame.format) {
        case PixelFormat::BGR8:
            bgr = cv::Mat(height, width, CV_8UC3, data, step);
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
            spdlog::warn("OpenCvThumbnailWriter: unsupported format {} for thumbnail",
                         sst::common::ToString(frame.format));
            return false;
    }

    std::error_code err;
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path(), err);
    }
    if (!cv::imwrite(output_path.string(), bgr)) {
        spdlog::error("OpenCvThumbnailWriter: imwrite({}) failed", output_path.string());
        return false;
    }
    return true;
}

}  // namespace sst::adapters::storage
