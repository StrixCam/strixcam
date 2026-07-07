#include "adapters/processing/gstreamer/vic-convert-stage.hpp"

#include <fmt/format.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <string>
#include <utility>

#include "domain/common/models/memory-type.hpp"
#include "domain/common/models/pixel-format.hpp"

namespace sst::adapters::processing {

namespace {

constexpr const char* kAppsrcName = "src";
constexpr const char* kAppsinkName = "sink";

// Bound on the synchronous push->pull round trip. The VIC converts a frame in
// low single-digit milliseconds; this only bites when the stage is broken, and
// then the failure counter below retires it entirely.
constexpr GstClockTime kPullTimeoutNs = 500 * GST_MSECOND;

// Consecutive push/pull failures before the stage declares itself unavailable
// so a misbehaving VIC costs a few bounded timeouts, not one per frame forever.
constexpr int kMaxConsecutiveFailures = 3;

// Copies the (possibly stride-padded) NV12 planes into a GstBuffer laid out per
// `info` (GStreamer's canonical NV12 strides/offsets for the declared caps).
auto FillNv12Buffer(const sst::capture::Frame& source, const GstVideoInfo& info,
                    GstBuffer* buffer) -> bool {
    GstMapInfo map{};
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE) == 0) {
        return false;
    }
    const auto width = source.geometry.width;
    const auto height = source.geometry.height;
    const auto& y_plane = source.planes[0];
    const auto& uv_plane = source.planes[1];

    const auto y_stride = static_cast<std::size_t>(GST_VIDEO_INFO_PLANE_STRIDE(&info, 0));
    const auto uv_stride = static_cast<std::size_t>(GST_VIDEO_INFO_PLANE_STRIDE(&info, 1));
    auto* y_dst = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 0);
    auto* uv_dst = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 1);

    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(y_dst + (row * y_stride),
                    y_plane.data + (static_cast<std::size_t>(row) * y_plane.stride), width);
    }
    for (std::uint32_t row = 0; row < height / 2; ++row) {
        std::memcpy(uv_dst + (row * uv_stride),
                    uv_plane.data + (static_cast<std::size_t>(row) * uv_plane.stride), width);
    }
    gst_buffer_unmap(buffer, &map);
    return true;
}

}  // namespace

VicConvertStage::VicConvertStage() {
    gst_init(nullptr, nullptr);
    // Probe once instead of paying a parse failure per pipeline build: off
    // Jetson (dev container) nvvidconv simply does not exist and the stage is
    // permanently a no-op — the caller's software path takes over.
    GstElementFactory* factory = gst_element_factory_find("nvvidconv");
    if (factory == nullptr) {
        unavailable_ = true;
        spdlog::info(
            "VicConvertStage: nvvidconv not present — postprocess stays on the software path");
        return;
    }
    gst_object_unref(factory);
}

VicConvertStage::~VicConvertStage() { Teardown(); }

auto VicConvertStage::IsAvailable() const -> bool { return !unavailable_; }

auto VicConvertStage::Teardown() -> void {
    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsrc_ != nullptr) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (appsink_ != nullptr) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    geometry_ = {};
}

auto VicConvertStage::RecordFailure(const char* what) -> void {
    ++consecutive_failures_;
    spdlog::warn("VicConvertStage: {} (failure {}/{})", what, consecutive_failures_,
                 kMaxConsecutiveFailures);
    Teardown();
    if (consecutive_failures_ >= kMaxConsecutiveFailures) {
        unavailable_ = true;
        spdlog::error(
            "VicConvertStage: {} consecutive failures — retiring the VIC hop, postprocess "
            "falls back to software permanently",
            consecutive_failures_);
    }
}

auto VicConvertStage::EnsurePipeline(const Geometry& geometry) -> bool {
    if (pipeline_ != nullptr && geometry_ == geometry) {
        return true;
    }
    Teardown();

    // System-memory in and out: appsrc pushes packed CPU NV12, nvvidconv hops
    // through NVMM internally for the VIC convert + scale, the appsink hands
    // back CPU BGRx (nvvidconv has no 24-bit BGR output; the cheap BGRx->BGR
    // repack stays with the caller). sync=false: this is a synchronous
    // converter, not a renderer.
    const std::string launch = fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true block=false "
        "caps=video/x-raw,format=NV12,width={iw},height={ih},framerate=0/1 "
        "! nvvidconv ! video/x-raw,format=BGRx,width={ow},height={oh} "
        "! appsink name={sink} sync=false max-buffers=2 drop=false enable-last-sample=false",
        fmt::arg("src", kAppsrcName), fmt::arg("iw", geometry.in_width),
        fmt::arg("ih", geometry.in_height), fmt::arg("ow", geometry.out_width),
        fmt::arg("oh", geometry.out_height), fmt::arg("sink", kAppsinkName));

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(launch.c_str(), &err);
    if (err != nullptr || pipeline_ == nullptr) {
        spdlog::warn("VicConvertStage: parse failed: {}",
                     err != nullptr ? err->message : "unknown");
        if (err != nullptr) {
            g_error_free(err);
        }
        Teardown();
        unavailable_ = true;
        return false;
    }
    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), kAppsrcName);
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), kAppsinkName);
    if (appsrc_ == nullptr || appsink_ == nullptr) {
        spdlog::warn("VicConvertStage: appsrc/appsink not found");
        Teardown();
        unavailable_ = true;
        return false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spdlog::warn("VicConvertStage: PLAYING transition failed");
        Teardown();
        unavailable_ = true;
        return false;
    }
    geometry_ = geometry;
    spdlog::info("VicConvertStage: NV12 {}x{} -> BGR {}x{} on VIC", geometry.in_width,
                 geometry.in_height, geometry.out_width, geometry.out_height);
    return true;
}

auto VicConvertStage::Convert(const sst::capture::Frame& source, std::uint32_t out_width,
                              std::uint32_t out_height) -> std::optional<cv::Mat> {
    if (unavailable_) {
        return std::nullopt;
    }
    if (source.format != sst::common::PixelFormat::NV12 ||
        source.memory != sst::common::MemoryType::CPU || source.planes.size() != 2 ||
        source.planes[0].data == nullptr || source.planes[1].data == nullptr ||
        source.geometry.width == 0 || source.geometry.height == 0 ||
        (source.geometry.width % 2) != 0 || (source.geometry.height % 2) != 0 || out_width == 0 ||
        out_height == 0) {
        return std::nullopt;
    }

    const Geometry geometry{.in_width = source.geometry.width,
                            .in_height = source.geometry.height,
                            .out_width = out_width,
                            .out_height = out_height};
    if (!EnsurePipeline(geometry)) {
        return std::nullopt;
    }

    GstVideoInfo in_info;
    gst_video_info_set_format(&in_info, GST_VIDEO_FORMAT_NV12,
                              static_cast<guint>(geometry.in_width),
                              static_cast<guint>(geometry.in_height));
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&in_info), nullptr);
    if (buffer == nullptr) {
        RecordFailure("buffer allocation failed");
        return std::nullopt;
    }
    if (!FillNv12Buffer(source, in_info, buffer)) {
        gst_buffer_unref(buffer);
        RecordFailure("buffer map failed");
        return std::nullopt;
    }

    // push_buffer takes ownership of `buffer`.
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer) != GST_FLOW_OK) {
        RecordFailure("push failed");
        return std::nullopt;
    }
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), kPullTimeoutNs);
    if (sample == nullptr) {
        RecordFailure("pull timed out");
        return std::nullopt;
    }

    // Map via GstVideoFrame so any VIC output alignment/stride padding
    // (advertised through GstVideoMeta) is honoured rather than assumed packed.
    std::optional<cv::Mat> result;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstVideoInfo out_info;
    if (caps != nullptr && gst_video_info_from_caps(&out_info, caps) != 0) {
        GstVideoFrame vframe;
        if (gst_video_frame_map(&vframe, &out_info, gst_sample_get_buffer(sample), GST_MAP_READ) !=
            0) {
            const cv::Mat bgrx(static_cast<int>(GST_VIDEO_FRAME_HEIGHT(&vframe)),
                               static_cast<int>(GST_VIDEO_FRAME_WIDTH(&vframe)), CV_8UC4,
                               GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0),
                               static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0)));
            // Single cheap channel-drop repack — this also copies the pixels out
            // of the GstBuffer, so the sample can be released immediately.
            cv::Mat bgr;
            cv::cvtColor(bgrx, bgr, cv::COLOR_BGRA2BGR);
            result = std::move(bgr);
            gst_video_frame_unmap(&vframe);
        }
    }
    gst_sample_unref(sample);

    if (!result) {
        RecordFailure("output map failed");
        return std::nullopt;
    }
    consecutive_failures_ = 0;
    return result;
}

}  // namespace sst::adapters::processing
