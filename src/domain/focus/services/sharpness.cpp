#include "domain/focus/services/sharpness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace sst::focus {

namespace {

// The sampled lattice spans at most this many points per axis, so the score
// costs the same for a 4K frame as for a preview frame.
constexpr std::uint32_t kMaxLatticeSpan = 96;
// Fewer Laplacian samples than this is statistically meaningless — bail.
constexpr std::size_t kMinSamples = 16;
// 4-neighbour discrete Laplacian: the center tap weighs 4, each neighbour -1.
constexpr double kLaplacianCenterWeight = 4.0;

// Formats whose plane 0 is a full-resolution 1-byte-per-pixel luma plane.
auto HasLeadingLumaPlane(sst::common::PixelFormat format) -> bool {
    switch (format) {
        case sst::common::PixelFormat::NV12:
        case sst::common::PixelFormat::I420:
        case sst::common::PixelFormat::GRAY8:
            return true;
        default:
            return false;
    }
}

}  // namespace

auto SharpnessScore(const sst::capture::Frame& frame) -> std::optional<double> {
    if (frame.memory != sst::common::MemoryType::CPU || !HasLeadingLumaPlane(frame.format) ||
        frame.planes.empty()) {
        return std::nullopt;
    }
    const auto& luma = frame.planes[0];
    const std::uint32_t width = frame.geometry.width;
    const std::uint32_t height = frame.geometry.height;
    if (luma.data == nullptr || width == 0 || height == 0) {
        return std::nullopt;
    }
    const std::uint32_t stride = luma.stride != 0 ? luma.stride : width;

    // Center crop: the middle half of the image per axis.
    const std::uint32_t crop_x = width / 4;
    const std::uint32_t crop_y = height / 4;
    const std::uint32_t crop_w = width / 2;
    const std::uint32_t crop_h = height / 2;
    // Decimation step per axis so the lattice never exceeds kMaxLatticeSpan.
    const std::uint32_t step_x = std::max(1U, crop_w / kMaxLatticeSpan);
    const std::uint32_t step_y = std::max(1U, crop_h / kMaxLatticeSpan);

    // Laplacian neighbours live one lattice step away, so the iterated range
    // shrinks by a step on each side. Everything below stays inside the crop.
    const std::uint32_t first_x = crop_x + step_x;
    const std::uint32_t first_y = crop_y + step_y;
    const std::uint32_t last_x = crop_x + crop_w - step_x;  // exclusive
    const std::uint32_t last_y = crop_y + crop_h - step_y;  // exclusive
    if (first_x >= last_x || first_y >= last_y) {
        return std::nullopt;
    }
    // The farthest byte any sample touches must exist in the plane.
    const std::size_t max_offset =
        static_cast<std::size_t>(crop_y + crop_h - 1) * stride + (crop_x + crop_w - 1);
    if (max_offset >= luma.size) {
        return std::nullopt;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    std::size_t count = 0;
    const std::uint8_t* data = luma.data;
    for (std::uint32_t py = first_y; py < last_y; py += step_y) {
        const std::uint8_t* row = data + static_cast<std::size_t>(py) * stride;
        const std::uint8_t* row_up = row - static_cast<std::size_t>(step_y) * stride;
        const std::uint8_t* row_down = row + static_cast<std::size_t>(step_y) * stride;
        for (std::uint32_t px = first_x; px < last_x; px += step_x) {
            const double center = row[px];
            const double laplacian = kLaplacianCenterWeight * center - row[px - step_x] -
                                     row[px + step_x] - row_up[px] - row_down[px];
            sum += laplacian;
            sum_sq += laplacian * laplacian;
            ++count;
        }
    }
    if (count < kMinSamples) {
        return std::nullopt;
    }
    const auto samples = static_cast<double>(count);
    const double mean = sum / samples;
    return (sum_sq / samples) - (mean * mean);
}

}  // namespace sst::focus
