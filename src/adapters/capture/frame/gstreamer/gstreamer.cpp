#include "./gstreamer.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstbuffer.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstmemory.h>
#include <gst/video/video.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/capture/ports/frame-src.hpp"
#include "domain/capture/models/camera-config.hpp"
#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/common/utils/get-timestamp.hpp"
#include "domain/config/utils/parse-model-version.hpp"

namespace sst::capture {

using sst::common::MemoryType;
using sst::common::PixelFormat;
using sst::common::utils::GetCurrentTimestamp;

GStreamerAdapter::GStreamerAdapter(const CameraConfig& camera_config, std::string device_model,
                                   std::uint16_t camera_index)
    : camera_config_(camera_config),
      device_model_(std::move(device_model)),
      camera_index_(camera_index) {
    gst_init(nullptr, nullptr);
}

auto GStreamerAdapter::CreatePipeline() -> std::string {
    const std::string sensor_id = std::to_string(camera_index_);
    const std::string width = std::to_string(camera_config_.width);
    const std::string height = std::to_string(camera_config_.height);
    const std::string fps = std::to_string(camera_config_.fps);
    const std::string format = sst::common::ToString(camera_config_.format);

    const std::optional<int> model_version = sst::config::ParseModelVersion(device_model_);
    if (!model_version) {
        spdlog::error("GStreamerAdapter: cannot parse device model '{}'", device_model_);
        return {};
    }

    // ISP tuning: hardware temporal-noise-reduction + edge-enhancement to fight
    // low-light grain (the ArduCAM module's stock .nito has NR mistuned, and JP7.2
    // blocks retuning). TNR/EE run in the ISP — no CPU cost. Cleaning the noise
    // also sharpens the H.264 output: x264 ultrafast stops spending bits on random
    // noise. One env var overrides the whole fragment so it can be dialed on-device
    // without a rebuild (e.g. lower TNR if fast motion smears).
    const char* isp_env = std::getenv("SST_ISP_TUNING");
    const std::string isp_tuning =
        (isp_env != nullptr) ? isp_env : "tnr-mode=2 tnr-strength=0.5 ee-mode=1 ee-strength=0.4";

    // Downstream frame-rate cap. The IMX477 only offers 1080p at 60fps (no 1080p30
    // sensor mode), so the sensor is driven at camera_config_.fps, but the whole
    // CPU-side pipeline — NV12->BGR postprocess (per camera), record x264, preview
    // x264 and the raw proxy — pays per delivered frame. At 60fps x2 cameras that
    // saturates the 6-core Orin Nano and every encoder starves (0-byte record,
    // blank preview). videorate drops to the target rate right after capture so the
    // expensive stages only see 30fps. Env-tunable so it can be raised on-device
    // once headroom is measured; never exceeds the sensor rate.
    constexpr int kDefaultPipelineFps = 30;
    const char* fps_env = std::getenv("SST_PIPELINE_FPS");
    int pipeline_fps = (fps_env != nullptr) ? std::atoi(fps_env) : kDefaultPipelineFps;
    if (pipeline_fps <= 0 || pipeline_fps > camera_config_.fps) {
        pipeline_fps = camera_config_.fps;
    }
    const std::string out_fps = std::to_string(pipeline_fps);

    std::string gst_pipeline;
    switch (*model_version) {
        case 1:
            gst_pipeline = "nvarguscamerasrc sensor-id=" + sensor_id + " " + isp_tuning +
                           " ! video/x-raw(memory:NVMM),width=" + width + ",height=" + height +
                           ",framerate=" + fps + "/1,format=NV12" +
                           " ! nvvidconv"
                           " ! video/x-raw,format=" +
                           format + " ! videorate drop-only=true max-rate=" + out_fps +
                           " ! video/x-raw,framerate=" + out_fps + "/1" +
                           " ! appsink name=" + gst_sink_name_ + " sync=false";
            break;
        default:
            spdlog::error("GStreamerAdapter: unsupported device model '{}' (v{})", device_model_,
                          *model_version);
            return {};
    }

    spdlog::info("GStreamerAdapter: sensor={} model={} format={} {}x{}@{}fps", sensor_id,
                 device_model_, format, width, height, fps);
    spdlog::info("GStreamerAdapter: pipeline: {}", gst_pipeline);

    return gst_pipeline;
}

auto GStreamerAdapter::CleanupPipeline() -> void {
    if (gst_sink_ != nullptr) {
        gst_object_unref(gst_sink_);
        gst_sink_ = nullptr;
    }
    if (gst_bus_ != nullptr) {
        gst_object_unref(gst_bus_);
        gst_bus_ = nullptr;
    }
    if (gst_pipeline_ != nullptr) {
        gst_object_unref(gst_pipeline_);
        gst_pipeline_ = nullptr;
    }
}

auto GStreamerAdapter::Start() -> void {
    if (is_running_) {
        return;
    }

    const std::string pipeline_str = CreatePipeline();
    if (pipeline_str.empty()) {
        spdlog::error("GStreamerAdapter: CreatePipeline() returned empty pipeline");
        return;
    }

    GError* gst_err = nullptr;
    gst_pipeline_ = gst_parse_launch(pipeline_str.c_str(), &gst_err);
    if (gst_err != nullptr) {
        spdlog::error("Failed to parse/link pipeline: {}", gst_err->message);
        g_error_free(gst_err);
        CleanupPipeline();
        return;
    }

    if (gst_pipeline_ == nullptr) {
        spdlog::error("Failed to create GStreamer pipeline from string");
        return;
    }

    gst_bus_ = gst_element_get_bus(gst_pipeline_);
    if (gst_bus_ == nullptr) {
        g_printerr("Failed to get bus from pipeline\n");
        CleanupPipeline();
        return;
    }

    gst_sink_ = gst_bin_get_by_name(GST_BIN(gst_pipeline_), gst_sink_name_.c_str());
    if (gst_sink_ == nullptr) {
        g_printerr("Failed to get appsink from pipeline\n");
        CleanupPipeline();
        return;
    }

    GstAppSink* gst_appsink = GST_APP_SINK(gst_sink_);
    gst_app_sink_set_drop(gst_appsink, 1);
    constexpr int kMaxBuffers = 5;
    gst_app_sink_set_max_buffers(gst_appsink, kMaxBuffers);
    gst_app_sink_set_emit_signals(gst_appsink, 0);

    const GstStateChangeReturn ret = gst_element_set_state(gst_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to PLAYING state\n");
        gst_element_set_state(gst_pipeline_, GST_STATE_NULL);
        CleanupPipeline();
        return;
    }

    // Prime pull decides the reported truth: only a pipeline that actually
    // delivered its first sample counts as running. Setting is_running_ before
    // this pull (the old behavior) let a stalled camera read healthy forever —
    // frame-truth health (U3) and the producer watchdog both key off IsRunning,
    // so a prime timeout must leave the adapter down, torn back to NULL, ready
    // for the watchdog's next Restart() (a cold nvargus-daemon recovers there).
    {
        constexpr GstClockTime kPrimeTimeout = 2 * GST_SECOND;

        GstSample* gstSample = gst_app_sink_try_pull_sample(gst_appsink, kPrimeTimeout);
        if (gstSample == nullptr) {
            spdlog::warn(
                "GStreamer: no initial frame within Start() prime timeout (sensor {}); leaving "
                "capture down for the watchdog",
                camera_index_);
            gst_element_set_state(gst_pipeline_, GST_STATE_NULL);
            CleanupPipeline();
            return;
        }
        RecordSampleNow();
        auto owner = std::shared_ptr<GstSample>(
            gstSample, [](GstSample* sample) { gst_sample_unref(sample); });

        auto capturedFrame = CreateFrameFromSample(owner.get());
        if (capturedFrame) {
            std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
            last_frame_ = *capturedFrame;
        }
    }
    is_running_ = true;
}

auto GStreamerAdapter::Stop() -> void {
    if (gst_pipeline_ != nullptr) {
        gst_element_set_state(gst_pipeline_, GST_STATE_NULL);
    }
    CleanupPipeline();

    {
        std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
        last_frame_.reset();
    }
    // Frame truth resets with the pipeline: a stopped/restarting camera reads
    // as stalled (RECOVERING) until frames actually resume after re-prime.
    last_sample_ns_ = kNoSample;

    is_running_ = false;
};

auto GStreamerAdapter::IsRunning() const -> bool {
    return is_running_ && (gst_pipeline_ != nullptr);
}

auto GStreamerAdapter::RecordSampleNow() -> void {
    last_sample_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
}

auto GStreamerAdapter::LastSampleAt() const
    -> std::optional<std::chrono::steady_clock::time_point> {
    const std::int64_t nanoseconds = last_sample_ns_;
    if (nanoseconds == kNoSample) {
        return std::nullopt;
    }
    return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanoseconds}};
}

auto GStreamerAdapter::HandleBusMessages() -> bool {
    if (gst_bus_ == nullptr) {
        return false;
    }
    while (GstMessage* msg = gst_bus_pop_filtered(
               gst_bus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* gerr = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);

            g_printerr("GStreamer ERROR: %s\n", ((gerr != nullptr) && (gerr->message != nullptr))
                                                    ? gerr->message
                                                    : "(null)");
            if (dbg != nullptr) {
                g_printerr("debug: %s\n", dbg);
            }

            if (gerr != nullptr) {
                g_error_free(gerr);
            }
            if (dbg != nullptr) {
                g_free(dbg);
            }

            gst_message_unref(msg);
            Stop();
            return false;
        }

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            gst_message_unref(msg);
            Stop();
            return false;
        }

        gst_message_unref(msg);
    }
    return true;
}

namespace {
// Owns the ref+map of a GstBuffer across CreateFrameFromSample's validation
// early-exits (previously seven hand-rolled unmap+unref pairs); Disarm() hands
// ownership over to the Frame's owner deleter, which performs the same
// unmap+unref when the last Frame copy drops.
class MappedBufferGuard {
   public:
    MappedBufferGuard(GstBuffer* buf, std::shared_ptr<GstMapInfo> map)
        : buf_(buf), map_(std::move(map)) {}
    ~MappedBufferGuard() {
        if (armed_) {
            gst_buffer_unmap(buf_, map_.get());
            gst_buffer_unref(buf_);
        }
    }
    MappedBufferGuard(const MappedBufferGuard&) = delete;
    auto operator=(const MappedBufferGuard&) -> MappedBufferGuard& = delete;
    MappedBufferGuard(MappedBufferGuard&&) = delete;
    auto operator=(MappedBufferGuard&&) -> MappedBufferGuard& = delete;

    auto Disarm() -> void { armed_ = false; }

   private:
    GstBuffer* buf_;
    std::shared_ptr<GstMapInfo> map_;
    bool armed_{true};
};
}  // namespace

auto GStreamerAdapter::CreateFrameFromSample(GstSample* gst_sample) -> std::optional<Frame> {
    if (gst_sample == nullptr) {
        return std::nullopt;
    }
    GstCaps* caps = gst_sample_get_caps(gst_sample);
    GstBuffer* buf = gst_sample_get_buffer(gst_sample);
    if (caps == nullptr || buf == nullptr) {
        return std::nullopt;
    }
    GstVideoInfo info;
    if (gst_video_info_from_caps(&info, caps) == 0) {
        return std::nullopt;
    }
    GstBuffer* buf_ref = gst_buffer_ref(buf);
    if (buf_ref == nullptr) {
        return std::nullopt;
    }
    auto map = std::make_shared<GstMapInfo>();
    if (gst_buffer_map(buf_ref, map.get(), GST_MAP_READ) == 0) {
        gst_buffer_unref(buf_ref);
        return std::nullopt;
    }
    // From here on every early exit must unmap+unref — one RAII guard instead
    // of a hand-rolled pair per validation branch.
    MappedBufferGuard guard(buf_ref, map);
    const guint number_of_planes = GST_VIDEO_INFO_N_PLANES(&info);
    if (number_of_planes == 0) {
        return std::nullopt;
    }
    std::vector<FramePlane> planes;
    planes.reserve(number_of_planes);
    const gsize mapped_size = map->size;
    const auto* mapped_data = static_cast<const uint8_t*>(map->data);
    if (mapped_data == nullptr || mapped_size == 0) {
        return std::nullopt;
    }
    for (guint i = 0; i < number_of_planes; ++i) {
        const auto stride = static_cast<guint>(GST_VIDEO_INFO_PLANE_STRIDE(&info, i));
        const gsize offset = GST_VIDEO_INFO_PLANE_OFFSET(&info, i);
        if (offset >= mapped_size) {
            return std::nullopt;
        }
        gsize current_plane_size = mapped_size;
        for (guint j = 0; j < number_of_planes; j++) {
            const gsize current_plane_offset = GST_VIDEO_INFO_PLANE_OFFSET(&info, j);
            if (current_plane_offset > offset && current_plane_offset < current_plane_size) {
                current_plane_size = current_plane_offset;
            }
        }
        if (current_plane_size <= offset) {
            return std::nullopt;
        }
        const gsize plane_size = current_plane_size - offset;
        if (plane_size == 0 || offset + plane_size > mapped_size) {
            return std::nullopt;
        }
        FramePlane plane{};
        plane.data = mapped_data + offset;
        plane.size = static_cast<std::size_t>(plane_size);
        plane.stride = stride;
        planes.push_back(plane);
    }

    Frame frame{};
    frame.frame_id = frame_counter_++;
    frame.captured_at = GetCurrentTimestamp();
    frame.geometry = FrameGeometry{
        .width = static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&info)),
        .height = static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)),
    };
    frame.planes = std::move(planes);

    const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&info);
    switch (fmt) {
        case GST_VIDEO_FORMAT_BGR:
            frame.format = PixelFormat::BGR8;
            break;
        case GST_VIDEO_FORMAT_BGRA:
            frame.format = PixelFormat::BGRA8;
            break;
        case GST_VIDEO_FORMAT_NV12:
            frame.format = PixelFormat::NV12;
            break;
        case GST_VIDEO_FORMAT_I420:
            frame.format = PixelFormat::I420;
            break;
        case GST_VIDEO_FORMAT_YUY2:
            frame.format = PixelFormat::YUYV;
            break;
        case GST_VIDEO_FORMAT_RGB:
            frame.format = PixelFormat::RGB8;
            break;
        case GST_VIDEO_FORMAT_RGBA:
            frame.format = PixelFormat::RGBA8;
            break;
        case GST_VIDEO_FORMAT_GRAY8:
            frame.format = PixelFormat::GRAY8;
            break;
        default:
            spdlog::error("GStreamerAdapter: unsupported GstVideoFormat {}", static_cast<int>(fmt));
            return std::nullopt;
    }

    frame.memory = MemoryType::CPU;
    guard.Disarm();  // the owner deleter below takes over the unmap+unref
    frame.owner = std::shared_ptr<void>(buf_ref, [map](void* gstreamerPipeline) {
        auto* gstreamerBuffer = static_cast<GstBuffer*>(gstreamerPipeline);
        gst_buffer_unmap(gstreamerBuffer, map.get());
        gst_buffer_unref(gstreamerBuffer);
    });
    return frame;
}

auto GStreamerAdapter::Capture() -> std::optional<Frame> {
    if (!is_running_ || (gst_pipeline_ == nullptr) || (gst_sink_ == nullptr) ||
        (gst_bus_ == nullptr)) {
        return std::nullopt;
    }
    if (!HandleBusMessages()) {
        return std::nullopt;
    }

    GstAppSink* appsink = GST_APP_SINK(gst_sink_);

    GstSample* captureGstSample = nullptr;
    GstSample* last = nullptr;

    // Drain any backlog non-blocking, keeping only the newest (latest wins).
    while ((captureGstSample = gst_app_sink_try_pull_sample(appsink, 0)) != nullptr) {
        if (last != nullptr) {
            gst_sample_unref(last);
        }
        last = captureGstSample;
    }

    // No backlog: BLOCK for the next frame rather than returning the cached one.
    // Returning last_frame_ here made the producer loop spin at CPU speed between
    // real frames — re-materializing (deep-copy) the same frame and pegging a core
    // per camera, starving the consumer + encoders (0-byte record, blank preview).
    // A bounded blocking pull paces the loop to the capture cadence; on timeout
    // return nullopt so the producer idles briefly instead of hot-looping.
    if (last == nullptr) {
        constexpr GstClockTime kCaptureBlockTimeout = 200 * GST_MSECOND;
        last = gst_app_sink_try_pull_sample(appsink, kCaptureBlockTimeout);
        if (last == nullptr) {
            return std::nullopt;
        }
    }
    // Frame truth for health: the sensor delivered a sample (regardless of
    // whether the conversion below succeeds).
    RecordSampleNow();

    auto sample =
        std::shared_ptr<GstSample>(last, [](GstSample* gstSample) { gst_sample_unref(gstSample); });
    auto fresh = CreateFrameFromSample(sample.get());
    if (!fresh) {
        std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
        return last_frame_;
    }

    {
        std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
        last_frame_ = *fresh;
    }
    return fresh;
}

}  // namespace sst::capture
