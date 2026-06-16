#include "adapters/processing/opencv/opencv-postprocessor.hpp"

#include <spdlog/spdlog.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>

#include "adapters/processing/opencv/frame-mat.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/crop-rect.hpp"
#include "domain/processing/models/postprocess-config.hpp"

namespace sst::adapters::processing {

OpenCvPostprocessor::OpenCvPostprocessor(sst::processing::PostprocessConfig config)
    : config_(config) {}

auto OpenCvPostprocessor::Process(const sst::capture::Frame& source,
                                  const sst::processing::CropRect& crop)
    -> std::optional<sst::capture::Frame> {
    if (source.format != sst::common::PixelFormat::NV12) {
        spdlog::warn("OpenCvPostprocessor: unsupported source format (only NV12 is supported)");
        return std::nullopt;
    }
    if (source.memory != sst::common::MemoryType::CPU) {
        spdlog::warn("OpenCvPostprocessor: only CPU memory is supported");
        return std::nullopt;
    }
    const auto src_width = source.geometry.width;
    const auto src_height = source.geometry.height;
    if (src_width == 0 || src_height == 0) {
        spdlog::warn("OpenCvPostprocessor: zero source geometry");
        return std::nullopt;
    }
    if (crop.width == 0 || crop.height == 0 || crop.x + crop.width > src_width ||
        crop.y + crop.height > src_height) {
        spdlog::warn("OpenCvPostprocessor: crop {{x={},y={},w={},h={}}} out of bounds for {}x{}",
                     crop.x, crop.y, crop.width, crop.height, src_width, src_height);
        return std::nullopt;
    }
    if (config_.output_width == 0 || config_.output_height == 0) {
        spdlog::warn("OpenCvPostprocessor: invalid output size {}x{}", config_.output_width,
                     config_.output_height);
        return std::nullopt;
    }
    if (config_.output_format != sst::common::PixelFormat::BGR8 &&
        config_.output_format != sst::common::PixelFormat::RGB8 &&
        config_.output_format != sst::common::PixelFormat::GRAY8) {
        spdlog::warn("OpenCvPostprocessor: unsupported output format");
        return std::nullopt;
    }

    auto nv12 = WrapNv12(source);
    if (!nv12) {
        spdlog::warn("OpenCvPostprocessor: WrapNv12 failed");
        return std::nullopt;
    }

    cv::Mat bgr;
    cv::cvtColor(*nv12, bgr, cv::COLOR_YUV2BGR_NV12);

    cv::Mat roi = bgr(cv::Rect(static_cast<int>(crop.x), static_cast<int>(crop.y),
                               static_cast<int>(crop.width), static_cast<int>(crop.height)));

    cv::Mat resized;
    cv::resize(
        roi, resized,
        cv::Size{static_cast<int>(config_.output_width), static_cast<int>(config_.output_height)},
        0, 0, cv::INTER_LINEAR);

    cv::Mat final_mat;
    switch (config_.output_format) {
        case sst::common::PixelFormat::BGR8:
            final_mat = resized;
            break;
        case sst::common::PixelFormat::RGB8:
            cv::cvtColor(resized, final_mat, cv::COLOR_BGR2RGB);
            break;
        case sst::common::PixelFormat::GRAY8:
            cv::cvtColor(resized, final_mat, cv::COLOR_BGR2GRAY);
            break;
        default:
            return std::nullopt;
    }

    return MakeOwnedFrame(final_mat, config_.output_format, source.frame_id, source.captured_at);
}

}  // namespace sst::adapters::processing
