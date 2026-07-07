#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "app/capture/ports/frame-src.hpp"
#include "domain/capture/models/camera-config.hpp"

extern "C" {
using GstElement = struct _GstElement;  // NOLINT(bugprone-reserved-identifier)
using GError = struct _GError;          // NOLINT(bugprone-reserved-identifier)
using GstBus = struct _GstBus;          // NOLINT(bugprone-reserved-identifier)
using GstSample = struct _GstSample;    // NOLINT(bugprone-reserved-identifier)
}

namespace sst::capture {

using sst::capture::Frame;
using sst::capture::ICaptureFrame;

class GStreamerAdapter final : public ICaptureFrame {
   public:
    GStreamerAdapter(const CameraConfig& camera_config, std::string device_model,
                     std::uint16_t camera_index);

    ~GStreamerAdapter() override = default;

    // Reports PRIMED truth: after Start(), IsRunning() is true only when the
    // pipeline reached PLAYING and delivered its first sample within the prime
    // timeout. A prime timeout (common against a cold nvargus-daemon) tears the
    // pipeline back down and leaves the adapter NOT running, so the producer
    // watchdog owns recovery instead of a stalled camera reading healthy.
    auto Start() -> void override;
    auto Stop() -> void override;
    auto IsRunning() const -> bool override;
    auto Capture() -> std::optional<Frame> override;
    // Steady-clock instant of the last sample the sensor delivered (prime or
    // Capture()); nullopt until the first sample after Start(). Reset by Stop()
    // so a restarting camera reads as stalled until frames actually resume.
    [[nodiscard]] auto LastSampleAt() const
        -> std::optional<std::chrono::steady_clock::time_point> override;

   private:
    // Sentinel for "no sample since Start()" in last_sample_ns_.
    static constexpr std::int64_t kNoSample = -1;

    auto RecordSampleNow() -> void;

    CameraConfig camera_config_;
    std::string device_model_;
    std::uint16_t camera_index_{0};
    std::uint64_t frame_counter_{0};
    // Read by IsRunning()/Capture() on the producer thread while Stop() (bus
    // error path) or the health readers touch it from others — atomic, not a
    // plain bool racing across threads.
    std::atomic<bool> is_running_{false};
    // Last-sample steady_clock nanoseconds (frame truth for health). Atomic so
    // the health derivation reads it lock-free against the producer's writes.
    std::atomic<std::int64_t> last_sample_ns_{kNoSample};
    std::string pipeline_str_;
    GstElement* gst_pipeline_{nullptr};
    GstBus* gst_bus_{nullptr};
    GError* gst_err_{nullptr};
    GstElement* gst_sink_{nullptr};
    std::string gst_sink_name_{"sink"};

    auto CleanupPipeline() -> void;
    auto HandleBusMessages() -> bool;
    auto CreateFrameFromSample(GstSample* gst_sample) -> std::optional<Frame>;
    auto CreatePipeline() -> std::string;

    mutable std::mutex last_frame_mtx_;
    std::optional<sst::capture::Frame> last_frame_;
};

}  // namespace sst::capture
