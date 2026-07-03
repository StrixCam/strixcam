#include "adapters/processing/opencv/opencv-postprocessor.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
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

// Below this delta a tuning parameter is treated as its identity value and the
// (CPU-costing) op is skipped — the common case where sliders sit at default.
constexpr float kTuningEpsilon = 0.001F;

OpenCvPostprocessor::OpenCvPostprocessor(sst::processing::PostprocessConfig config,
                                         const sst::processing::ColorCalibrationState* calibration,
                                         sst::processing::FrameColorStats* frame_stats)
    : config_(config), calibration_(calibration), frame_stats_(frame_stats) {}

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

    // Sample the PRE-correction average (BGR) for auto-white-balance. Measuring
    // before the gain means the auto-WB handler sees the raw cast, not an
    // already-corrected frame. cv::mean returns Scalar(B,G,R).
    if (frame_stats_ != nullptr) {
        cv::Scalar mean;
        cv::Scalar stddev;
        cv::meanStdDev(resized, mean, stddev);
        const auto spread = static_cast<float>((stddev[0] + stddev[1] + stddev[2]) / 3.0);
        frame_stats_->Set(static_cast<float>(mean[0]), static_cast<float>(mean[1]),
                          static_cast<float>(mean[2]), spread);
    }

    // Per-channel white-balance correction (fixes the ArduCAM IMX477 magenta
    // cast at the ISP tuning level we can't reach on JetPack 7.2). Applied on the
    // downscaled BGR — cheap, and before any RGB/GRAY convert so every output
    // format inherits neutral color. uint8 multiply saturates, so gains>1 clip
    // rather than wrap. Scalar is BGR order. Live gains from the calibration state
    // (diagnostic sliders) win over the static config when wired.
    const auto wb = (calibration_ != nullptr)
                        ? calibration_->Get()
                        : sst::processing::ColorCalibrationState::Gains{
                              config_.color_correction.r_gain, config_.color_correction.g_gain,
                              config_.color_correction.b_gain, config_.color_correction.enabled};
    if (wb.enabled) {
        cv::multiply(resized, cv::Scalar(wb.b, wb.g, wb.r), resized);
        // Saturation: lerp between the frame and its greyscale (1.0 = unchanged,
        // 0 = greyscale, >1 = more vivid). Fixes the ArduCAM's washed-out look.
        if (std::fabs(wb.saturation - 1.0F) > kTuningEpsilon) {
            cv::Mat grey;
            cv::cvtColor(resized, grey, cv::COLOR_BGR2GRAY);
            cv::cvtColor(grey, grey, cv::COLOR_GRAY2BGR);
            cv::addWeighted(resized, wb.saturation, grey, 1.0 - wb.saturation, 0.0, resized);
        }
        // Contrast (scaled around mid-grey) + brightness (additive). convertTo
        // saturates to uint8, so out-of-range values clip rather than wrap.
        if (std::fabs(wb.contrast - 1.0F) > kTuningEpsilon ||
            std::fabs(wb.brightness) > kTuningEpsilon) {
            const double beta = (128.0 * (1.0 - wb.contrast)) + (wb.brightness * 255.0);
            resized.convertTo(resized, -1, wb.contrast, beta);
        }
    }

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
