#include "adapters/processing/opencv/opencv-preprocessor.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <utility>

#include "adapters/processing/opencv/frame-mat.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/processing/models/color-mode.hpp"
#include "domain/processing/models/frame-bundle.hpp"
#include "domain/processing/models/preprocess-config.hpp"

namespace sst::adapters::processing {

namespace {
// Output value assigned to above-threshold pixels by cv::THRESH_BINARY
// (full-scale white for an 8-bit single-channel image).
constexpr double kBinaryMaxValue = 255.0;
}  // namespace

OpenCvPreprocessor::OpenCvPreprocessor(sst::processing::PreprocessConfig config)
    : config_(config) {}

auto OpenCvPreprocessor::Process(const sst::capture::Frame& raw)
    -> std::optional<sst::processing::FrameBundle> {
    if (raw.format != sst::common::PixelFormat::NV12) {
        spdlog::warn("OpenCvPreprocessor: unsupported input format (only NV12 is supported)");
        return std::nullopt;
    }
    if (raw.memory != sst::common::MemoryType::CPU) {
        spdlog::warn("OpenCvPreprocessor: only CPU memory is supported");
        return std::nullopt;
    }
    if (raw.planes.size() != 2) {
        spdlog::warn("OpenCvPreprocessor: NV12 must have 2 planes, got {}", raw.planes.size());
        return std::nullopt;
    }
    const auto width = raw.geometry.width;
    const auto height = raw.geometry.height;
    if (width == 0 || height == 0 || (width % 2) != 0 || (height % 2) != 0) {
        spdlog::warn("OpenCvPreprocessor: invalid NV12 geometry {}x{}", width, height);
        return std::nullopt;
    }
    if (config_.ai_width == 0 || config_.ai_height == 0) {
        spdlog::warn("OpenCvPreprocessor: invalid AI target size {}x{}", config_.ai_width,
                     config_.ai_height);
        return std::nullopt;
    }

    const cv::Size ai_size{static_cast<int>(config_.ai_width), static_cast<int>(config_.ai_height)};

    cv::Mat ai_mat;
    sst::common::PixelFormat ai_format = sst::common::PixelFormat::GRAY8;

    switch (config_.ai_color_mode) {
        case sst::processing::ColorMode::Grayscale: {
            // Y plane *is* the luminance — wrap directly, no color conversion.
            cv::Mat y_full(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                           const_cast<std::uint8_t*>(raw.planes[0].data), raw.planes[0].stride);
            cv::resize(y_full, ai_mat, ai_size, 0, 0, cv::INTER_AREA);
            ai_format = sst::common::PixelFormat::GRAY8;
            break;
        }
        case sst::processing::ColorMode::Binary: {
            cv::Mat y_full(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                           const_cast<std::uint8_t*>(raw.planes[0].data), raw.planes[0].stride);
            cv::Mat resized;
            cv::resize(y_full, resized, ai_size, 0, 0, cv::INTER_AREA);
            cv::threshold(resized, ai_mat, static_cast<double>(config_.binary_threshold),
                          kBinaryMaxValue, cv::THRESH_BINARY);
            ai_format = sst::common::PixelFormat::GRAY8;
            break;
        }
        case sst::processing::ColorMode::RGB: {
            auto nv12 = WrapNv12(raw);
            if (!nv12) {
                spdlog::warn("OpenCvPreprocessor: WrapNv12 failed");
                return std::nullopt;
            }
            cv::Mat rgb_full;
            cv::cvtColor(*nv12, rgb_full, cv::COLOR_YUV2RGB_NV12);
            cv::resize(rgb_full, ai_mat, ai_size, 0, 0, cv::INTER_AREA);
            ai_format = sst::common::PixelFormat::RGB8;
            break;
        }
    }

    sst::processing::FrameBundle bundle;
    bundle.ai_frame = MakeOwnedFrame(ai_mat, ai_format, raw.frame_id, raw.captured_at);
    bundle.source_frame = raw;  // Value copy; bumps `owner` refcount, no pixel copy.
    return bundle;
}

}  // namespace sst::adapters::processing
