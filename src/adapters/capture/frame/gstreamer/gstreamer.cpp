#include "./gstreamer.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstbuffer.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstmemory.h>
#include <gst/video/video.h>
#include <spdlog/spdlog.h>

#include <memory>
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

    std::string gst_pipeline;
    switch (*model_version) {
        case 1:
            gst_pipeline = "nvarguscamerasrc sensor-id=" + sensor_id +
                           " ! video/x-raw(memory:NVMM),width=" + width + ",height=" + height +
                           ",framerate=" + fps + "/1,format=NV12" +
                           " ! nvvidconv"
                           " ! video/x-raw,format=" +
                           format + " ! appsink name=" + gst_sink_name_ + " sync=false";
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
    if (gst_err_ != nullptr) {
        g_error_free(gst_err_);
        gst_err_ = nullptr;
    }
}

auto GStreamerAdapter::Start() -> void {
    if (is_running_) {
        return;
    }

    pipeline_str_ = CreatePipeline();
    if (pipeline_str_.empty()) {
        spdlog::error("GStreamerAdapter: CreatePipeline() returned empty pipeline");
        return;
    }

    gst_err_ = nullptr;
    gst_pipeline_ = gst_parse_launch(pipeline_str_.c_str(), &gst_err_);
    if (gst_err_ != nullptr) {
        spdlog::error("Failed to parse/link pipeline: {}", gst_err_->message);
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

    is_running_ = true;
    {
        constexpr GstClockTime kPrimeTimeout = 2 * GST_SECOND;

        GstSample* gstSample = gst_app_sink_try_pull_sample(gst_appsink, kPrimeTimeout);
        if (gstSample != nullptr) {
            auto owner = std::shared_ptr<GstSample>(
                gstSample, [](GstSample* sample) { gst_sample_unref(sample); });

            auto capturedFrame = CreateFrameFromSample(owner.get());
            if (capturedFrame) {
                std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
                last_frame_ = *capturedFrame;
            }
        } else {
            spdlog::warn("GStreamer: did not receive initial frame during Start() prime");
        }
    }
}

auto GStreamerAdapter::Stop() -> void {
    if (gst_pipeline_ != nullptr) {
        gst_element_set_state(gst_pipeline_, GST_STATE_NULL);
    }
    CleanupPipeline();

    std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
    last_frame_.reset();

    is_running_ = false;
};

auto GStreamerAdapter::IsRunning() const -> bool {
    return is_running_ && (gst_pipeline_ != nullptr);
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
    const guint number_of_planes = GST_VIDEO_INFO_N_PLANES(&info);
    if (number_of_planes == 0) {
        gst_buffer_unmap(buf_ref, map.get());
        gst_buffer_unref(buf_ref);
        return std::nullopt;
    }
    std::vector<FramePlane> planes;
    planes.reserve(number_of_planes);
    const gsize mapped_size = map->size;
    const auto* mapped_data = static_cast<const uint8_t*>(map->data);
    if (mapped_data == nullptr || mapped_size == 0) {
        gst_buffer_unmap(buf_ref, map.get());
        gst_buffer_unref(buf_ref);
        return std::nullopt;
    }
    for (guint i = 0; i < number_of_planes; ++i) {
        const auto stride = static_cast<guint>(GST_VIDEO_INFO_PLANE_STRIDE(&info, i));
        const gsize offset = GST_VIDEO_INFO_PLANE_OFFSET(&info, i);
        if (offset >= mapped_size) {
            gst_buffer_unmap(buf_ref, map.get());
            gst_buffer_unref(buf_ref);
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
            gst_buffer_unmap(buf_ref, map.get());
            gst_buffer_unref(buf_ref);
            return std::nullopt;
        }
        const gsize plane_size = current_plane_size - offset;
        if (plane_size == 0 || offset + plane_size > mapped_size) {
            gst_buffer_unmap(buf_ref, map.get());
            gst_buffer_unref(buf_ref);
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
            gst_buffer_unmap(buf_ref, map.get());
            gst_buffer_unref(buf_ref);
            return std::nullopt;
    }

    frame.memory = MemoryType::CPU;
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

    while ((captureGstSample = gst_app_sink_try_pull_sample(appsink, 0)) != nullptr) {
        if (last != nullptr) {
            gst_sample_unref(last);
        }
        last = captureGstSample;
    }

    if (last == nullptr) {
        std::lock_guard<std::mutex> lastFrameLock(last_frame_mtx_);
        return last_frame_;
    }

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
