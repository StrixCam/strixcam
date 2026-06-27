#include "adapters/processing/opencv/opencv-side-by-side-compositor.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "adapters/processing/opencv/frame-mat.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace sst::adapters::processing {

namespace {

constexpr int kBgrChannels = 3;

// Wrap a compact BGR8 Frame (single tightly-packed plane) as a borrowing
// cv::Mat. Returns an empty Mat on any shape/format mismatch.
auto WrapBgr(const sst::capture::Frame& frame) -> cv::Mat {
    if (frame.format != sst::common::PixelFormat::BGR8 || frame.planes.empty() ||
        frame.planes[0].data == nullptr || frame.geometry.width == 0 ||
        frame.geometry.height == 0) {
        return {};
    }
    const auto width = static_cast<int>(frame.geometry.width);
    const auto height = static_cast<int>(frame.geometry.height);
    const auto stride = frame.planes[0].stride != 0
                            ? static_cast<std::size_t>(frame.planes[0].stride)
                            : static_cast<std::size_t>(width) * kBgrChannels;
    // const_cast: cv::Mat needs a non-const pointer, but the wrap is read-only —
    // the source pixels are only ever copied out (resize into an owned column).
    cv::Mat wrapped(height, width, CV_8UC3,
                    const_cast<std::uint8_t*>(
                        frame.planes[0].data),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
                    stride);
    return wrapped;
}

// Letterbox `src` into a `col_width` x `col_height` black canvas, preserving
// aspect ratio (centered). An empty `src` yields an all-black column.
auto LetterboxColumn(const cv::Mat& src, int col_width, int col_height) -> cv::Mat {
    cv::Mat column = cv::Mat::zeros(col_height, col_width, CV_8UC3);
    if (src.empty()) {
        return column;
    }
    const double scale = std::min(static_cast<double>(col_width) / src.cols,
                                  static_cast<double>(col_height) / src.rows);
    const int scaled_width = std::max(1, static_cast<int>(src.cols * scale));
    const int scaled_height = std::max(1, static_cast<int>(src.rows * scale));
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(scaled_width, scaled_height), 0, 0, cv::INTER_AREA);
    const int x_offset = (col_width - scaled_width) / 2;
    const int y_offset = (col_height - scaled_height) / 2;
    resized.copyTo(column(cv::Rect(x_offset, y_offset, scaled_width, scaled_height)));
    return column;
}

}  // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: output canvas
// width/height; passed by name (kOverlayWidth, kOverlayHeight) at the single call site
OpenCvSideBySideCompositor::OpenCvSideBySideCompositor(std::uint32_t output_width,
                                                       std::uint32_t output_height)
    // NOLINTEND(bugprone-easily-swappable-parameters)
    : output_width_(output_width), output_height_(output_height) {}

auto OpenCvSideBySideCompositor::CompositeSideBySide(const sst::capture::Frame& left,
                                                     const sst::capture::Frame& right)
    -> std::optional<sst::capture::Frame> {
    const cv::Mat left_mat = WrapBgr(left);
    const cv::Mat right_mat = WrapBgr(right);
    if (left_mat.empty() && right_mat.empty()) {
        spdlog::warn("OpenCvSideBySideCompositor: both inputs invalid (need BGR8); skipping");
        return std::nullopt;
    }

    const int canvas_height = static_cast<int>(output_height_);
    // Split the canvas evenly; an odd width gives the right column the extra
    // pixel so the two columns still sum to output_width_ exactly.
    const int left_width = static_cast<int>(output_width_) / 2;
    const int right_width = static_cast<int>(output_width_) - left_width;

    const cv::Mat left_col = LetterboxColumn(left_mat, left_width, canvas_height);
    const cv::Mat right_col = LetterboxColumn(right_mat, right_width, canvas_height);

    cv::Mat composite;
    cv::hconcat(left_col, right_col, composite);

    // frame_id / timestamp follow the left (chosen) camera so downstream PTS
    // stamping stays monotonic with the single-camera path.
    return MakeOwnedFrame(composite, sst::common::PixelFormat::BGR8, left.frame_id,
                          left.captured_at);
}

}  // namespace sst::adapters::processing
