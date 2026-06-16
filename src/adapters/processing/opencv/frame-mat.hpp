#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/common/models/timestamp.hpp"

namespace sst::adapters::processing {

// Wrap an NV12 Frame as a single (H*3/2, W) CV_8UC1 cv::Mat — the layout
// OpenCV's cv::COLOR_YUV2BGR_NV12 / cv::COLOR_YUV2RGB_NV12 expects: H rows
// of Y followed by H/2 rows of interleaved UV at the same row stride.
//
// Fast path (no copy): if Y/UV are tightly contiguous (planes[0].data +
// planes[0].size == planes[1].data) and both planes have stride == W, returns
// a non-owning Mat referencing the Frame's memory. The caller must keep `f`
// alive while using the Mat.
//
// Fallback: if planes are non-contiguous or padded, allocates a fresh
// (H*3/2, W) CV_8UC1 Mat and row-copies Y and UV into it.
//
// Returns std::nullopt on invalid input: wrong format, wrong plane count,
// zero or odd geometry.
auto WrapNv12(const sst::capture::Frame& frame) -> std::optional<cv::Mat>;

// Build a Frame whose `owner` shared_ptr pins a freshly allocated
// std::vector<uint8_t> that holds a deep copy of `mat`.
//
// The Frame is laid out compactly: a single plane with
// stride = mat.cols * mat.channels() (no row padding) and
// size = rows * cols * channels. Handles padded input Mats correctly by
// row-copying.
//
// Format validity is the caller's responsibility (e.g. don't pass a 1-channel
// mat with fmt = BGR8). Non-CV_8U mats are rejected via assertion.
auto MakeOwnedFrame(const cv::Mat& mat, sst::common::PixelFormat fmt, std::uint64_t frame_id,
                    sst::common::Timestamp captured_at) -> sst::capture::Frame;

}  // namespace sst::adapters::processing
