#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>

#include "domain/capture/models/frame.hpp"

// Forward-declared to keep the GStreamer headers out of this header; the .cpp
// includes <gst/gst.h>.
// NOLINTNEXTLINE(bugprone-reserved-identifier) floor-ok: _GstElement is GStreamer's own struct tag
using GstElement = struct _GstElement;

namespace sst::adapters::processing {

// VIC-offloaded NV12 -> BGR convert + scale: an internal
// `appsrc ! nvvidconv ! appsink` hop the postprocess adapter delegates its
// per-frame colour conversion to, so the heavy YUV matrix + resample run on the
// Jetson VIC instead of the CPU (the postprocess is the biggest per-frame CPU
// cost feeding every encode branch). OpenCV keeps only the calibration math on
// the already-converted buffer.
//
// Capability-gated with a software fallback, never #ifdef'd: `nvvidconv` does
// not exist off-Jetson (dev container), so the stage probes the element factory
// at construction and reports unavailable — the caller (OpenCvPostprocessor)
// then stays on the proven full-OpenCV path. A mid-run VIC failure (push/pull
// error) degrades the same way after a bounded number of consecutive failures.
//
// NOT thread-safe: one converter per postprocessor, driven from the single
// pipeline consumer thread (the same discipline as the postprocessor itself).
class VicConvertStage final {
   public:
    VicConvertStage();
    ~VicConvertStage();

    VicConvertStage(const VicConvertStage&) = delete;
    auto operator=(const VicConvertStage&) -> VicConvertStage& = delete;
    VicConvertStage(VicConvertStage&&) = delete;
    auto operator=(VicConvertStage&&) -> VicConvertStage& = delete;

    // Convert + scale a CPU NV12 frame to a BGR (CV_8UC3) Mat of
    // out_width x out_height on the VIC. nullopt when the stage is unavailable
    // (no nvvidconv — non-Jetson container), the input is not usable NV12, or
    // the push/pull round-trip fails — the caller falls back to software.
    auto Convert(const sst::capture::Frame& source, std::uint32_t out_width,
                 std::uint32_t out_height) -> std::optional<cv::Mat>;

    // False when nvvidconv is missing or the stage gave up after repeated
    // failures (Convert will always return nullopt).
    [[nodiscard]] auto IsAvailable() const -> bool;

   private:
    // The internal pipeline is keyed on the exact in/out geometry; a change
    // (camera mode switch, output reconfig) tears down and rebuilds it.
    struct Geometry {
        std::uint32_t in_width{0};
        std::uint32_t in_height{0};
        std::uint32_t out_width{0};
        std::uint32_t out_height{0};

        friend auto operator==(const Geometry&, const Geometry&) -> bool = default;
    };

    auto EnsurePipeline(const Geometry& geometry) -> bool;
    auto Teardown() -> void;
    auto RecordFailure(const char* what) -> void;

    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    GstElement* appsink_{nullptr};
    Geometry geometry_{};
    bool unavailable_{false};
    int consecutive_failures_{0};
};

}  // namespace sst::adapters::processing
