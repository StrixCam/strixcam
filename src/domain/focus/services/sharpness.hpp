#pragma once

#include <optional>

#include "domain/capture/models/frame.hpp"

namespace sst::focus {

// Contrast-based sharpness score for autofocus: the variance of a 4-neighbour
// Laplacian sampled over a DOWNSCALED CENTER CROP of the frame's luma plane.
// Higher = sharper; the absolute value is meaningless across scenes (the AF
// loop only compares scores of the same scene at different lens positions).
//
// Cost is bounded regardless of frame size: the center crop (middle half of
// the image, where the subject the user is framing lives) is sampled on a
// decimated lattice of at most ~kMaxLatticeSpan² points, so a 4K frame costs
// the same as a preview frame. Pure C++ on the Y plane — no OpenCV, no color
// conversion.
//
// Returns std::nullopt when the frame has no scoreable luma: non-CPU memory,
// a format without a leading 1-byte-per-pixel luma plane (only NV12 / I420 /
// GRAY8 qualify), a null/undersized plane, or a crop too small to sample.
auto SharpnessScore(const sst::capture::Frame& frame) -> std::optional<double>;

}  // namespace sst::focus
