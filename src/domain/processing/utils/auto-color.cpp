#include "domain/processing/utils/auto-color.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace sst::processing {

namespace {

// Statistics pass, not a render: every 4th row/column is plenty for a frame
// average and keeps the cost negligible at the loop's low tick rate.
constexpr std::uint32_t kSampleStep = 4;

// BT.601 full-range YUV -> RGB (the convention behind the postprocessor's
// NV12->BGR convert). Linear, so it maps plane MEANS directly.
constexpr float kChromaBias = 128.0F;
constexpr float kRFromV = 1.403F;
constexpr float kGFromU = -0.344F;
constexpr float kGFromV = -0.714F;
constexpr float kBFromU = 1.773F;
constexpr float kChannelMax = 255.0F;

// The sampling window of one plane: dimensions in SAMPLES (not bytes), with
// the byte layout of one sample row (pixel_stride bytes apart, first sample
// `offset` bytes in — {2, 0}/{2, 1} pick U/V out of the interleaved NV12
// chroma plane).
struct PlaneWindow {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pixel_stride;
    std::uint32_t offset;
};

auto PlaneMean(const sst::capture::FramePlane& plane,
               const PlaneWindow& window) -> std::optional<float> {
    if (plane.data == nullptr || window.width == 0 || window.height == 0) {
        return std::nullopt;
    }
    double sum = 0.0;
    std::size_t count = 0;
    for (std::uint32_t row = 0; row < window.height; row += kSampleStep) {
        const std::size_t row_base = static_cast<std::size_t>(row) * plane.stride;
        for (std::uint32_t col = 0; col < window.width; col += kSampleStep) {
            const std::size_t index =
                row_base + (static_cast<std::size_t>(col) * window.pixel_stride) + window.offset;
            if (index >= plane.size) {
                return std::nullopt;  // stride/geometry mismatch — don't trust the frame
            }
            sum += plane.data[index];
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return static_cast<float>(sum / static_cast<double>(count));
}

}  // namespace

auto MeasureFrameMeans(const sst::capture::Frame& frame) -> std::optional<ChannelMeans> {
    if (frame.format != sst::common::PixelFormat::NV12 ||
        frame.memory != sst::common::MemoryType::CPU || frame.planes.size() < 2 ||
        frame.geometry.width < 2 || frame.geometry.height < 2) {
        return std::nullopt;
    }
    const auto width = frame.geometry.width;
    const auto height = frame.geometry.height;
    const auto mean_y = PlaneMean(
        frame.planes[0], {.width = width, .height = height, .pixel_stride = 1, .offset = 0});
    // UV plane: half resolution, interleaved U,V byte pairs.
    const auto mean_u =
        PlaneMean(frame.planes[1],
                  {.width = width / 2, .height = height / 2, .pixel_stride = 2, .offset = 0});
    const auto mean_v =
        PlaneMean(frame.planes[1],
                  {.width = width / 2, .height = height / 2, .pixel_stride = 2, .offset = 1});
    if (!mean_y || !mean_u || !mean_v) {
        return std::nullopt;
    }
    const float chroma_u = *mean_u - kChromaBias;
    const float chroma_v = *mean_v - kChromaBias;
    ChannelMeans means;
    means.r = std::clamp(*mean_y + (kRFromV * chroma_v), 0.0F, kChannelMax);
    means.g = std::clamp(*mean_y + (kGFromU * chroma_u) + (kGFromV * chroma_v), 0.0F, kChannelMax);
    means.b = std::clamp(*mean_y + (kBFromU * chroma_u), 0.0F, kChannelMax);
    return means;
}

auto SharedNeutralTarget(std::span<const std::optional<ChannelMeans>> means)
    -> std::optional<float> {
    float sum = 0.0F;
    std::size_t count = 0;
    for (const auto& sample : means) {
        if (sample.has_value()) {
            sum += sample->Luma();
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return sum / static_cast<float>(count);
}

auto GreyWorldStep(const ColorCalibrationState::Gains& current, const ChannelMeans& means,
                   float target,
                   const AutoColorTuning& tuning) -> std::optional<ColorCalibrationState::Gains> {
    if (means.r < tuning.min_usable_mean || means.g < tuning.min_usable_mean ||
        means.b < tuning.min_usable_mean || target <= 0.0F) {
        return std::nullopt;  // too dark to trust a grey-world estimate
    }
    const auto ideal = [&](float mean) {
        return std::clamp(target / mean, tuning.min_gain, tuning.max_gain);
    };
    const float ideal_r = ideal(means.r);
    const float ideal_g = ideal(means.g);
    const float ideal_b = ideal(means.b);

    // Dead-band on the REMAINING error: once every channel's ideal gain sits
    // within dead_band of what is already applied, the loop is converged and
    // writes nothing — scene noise can never make it visibly pump.
    const float error = std::max({std::fabs(ideal_r - current.r), std::fabs(ideal_g - current.g),
                                  std::fabs(ideal_b - current.b)});
    if (error < tuning.dead_band) {
        return std::nullopt;
    }

    ColorCalibrationState::Gains next = current;
    next.r = current.r + (tuning.alpha * (ideal_r - current.r));
    next.g = current.g + (tuning.alpha * (ideal_g - current.g));
    next.b = current.b + (tuning.alpha * (ideal_b - current.b));
    next.enabled = true;
    return next;
}

}  // namespace sst::processing
