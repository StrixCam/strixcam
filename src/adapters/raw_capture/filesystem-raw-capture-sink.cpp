#include "adapters/raw_capture/filesystem-raw-capture-sink.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <system_error>
#include <utility>

#include "adapters/raw_capture/proxy-launch.hpp"
#include "domain/raw_capture/models/raw-capture-identity.hpp"
#include "domain/raw_capture/services/raw-capture-naming.hpp"

namespace sst::adapters::raw_capture {

namespace {
// Bound the wait for mp4mux to flush its moov on Stop so a stuck proxy pipeline
// can't hang shutdown; the leaky pre-encoder queue caps how large the residual
// backlog can get. Matches the record recorder's finalize budget.
constexpr int kFinalizeTimeoutSeconds = 10;

// Nominal source framerate for the appsrc input caps. do-timestamp=true supplies
// the real PTS and the pipeline's videorate conforms to the proxy fps, so this
// is only a caps hint — the true capture cadence varies (30/60).
constexpr int kProxySourceFramerateHint = 30;

auto GstFormatFor(sst::common::PixelFormat fmt) -> const char* {
    switch (fmt) {
        case sst::common::PixelFormat::BGR8:
            return "BGR";
        case sst::common::PixelFormat::RGB8:
            return "RGB";
        case sst::common::PixelFormat::BGRA8:
            return "BGRA";
        case sst::common::PixelFormat::RGBA8:
            return "RGBA";
        case sst::common::PixelFormat::GRAY8:
            return "GRAY8";
        case sst::common::PixelFormat::NV12:
            return "NV12";
        case sst::common::PixelFormat::I420:
            return "I420";
        case sst::common::PixelFormat::YUYV:
            return "YUY2";
    }
    return "NV12";
}
}  // namespace

FilesystemRawCaptureSink::FilesystemRawCaptureSink(std::filesystem::path video_dir,
                                                   std::uint32_t camera_count)
    : video_dir_(std::move(video_dir)),
      camera_count_(camera_count),
      // VIC offload on by default; SST_DISABLE_VIC=1 forces software scale/convert
      // (matches the record/RTMP paths so one switch toggles the whole box).
      use_vic_(std::getenv("SST_DISABLE_VIC") == nullptr) {
    gst_init(nullptr, nullptr);
}

FilesystemRawCaptureSink::~FilesystemRawCaptureSink() {
    if (capturing_.load()) {
        (void)Stop();
    }
}

auto FilesystemRawCaptureSink::Start(const std::string& capture_group_id) -> bool {
    std::lock_guard lock(mtx_);
    if (capturing_.load()) {
        spdlog::warn("RawCaptureSink::Start: already capturing");
        return false;
    }
    if (capture_group_id.empty()) {
        spdlog::error("RawCaptureSink::Start: empty capture_group_id");
        return false;
    }

    std::error_code err;
    std::filesystem::create_directories(video_dir_, err);
    if (err) {
        spdlog::error("RawCaptureSink::Start: cannot create {}: {}", video_dir_.string(),
                      err.message());
        return false;
    }

    // Group-id collision guard: reject a reused capture_group_id rather than
    // truncate-overwrite a prior group's (possibly not-yet-downloaded) proxy
    // files. The app mints UUIDs so a collision means a retry/reconnect resend —
    // fail loudly instead of silently destroying training footage.
    for (std::uint32_t i = 0; i < camera_count_; ++i) {
        const auto name =
            sst::raw_capture::raw_capture_naming::FileName(sst::raw_capture::RawCaptureIdentity{
                .capture_group_id = capture_group_id, .camera_index = i});
        if (std::filesystem::exists(video_dir_ / name)) {
            spdlog::error("RawCaptureSink::Start: group {} already has files on disk — refusing",
                          capture_group_id);
            return false;
        }
    }

    std::vector<std::unique_ptr<CameraWriter>> writers;
    writers.reserve(camera_count_);
    for (std::uint32_t i = 0; i < camera_count_; ++i) {
        const auto name =
            sst::raw_capture::raw_capture_naming::FileName(sst::raw_capture::RawCaptureIdentity{
                .capture_group_id = capture_group_id, .camera_index = i});
        const auto path = video_dir_ / name;

        const std::string desc = BuildProxyLaunch(path.string(), use_vic_);
        GError* gerr = nullptr;
        GstElement* pipeline = gst_parse_launch(desc.c_str(), &gerr);
        if (pipeline == nullptr || gerr != nullptr) {
            spdlog::error("RawCaptureSink::Start: parse failed for cam{}: {}", i,
                          gerr != nullptr ? gerr->message : "unknown");
            if (gerr != nullptr) {
                g_error_free(gerr);
            }
            if (pipeline != nullptr) {
                gst_object_unref(pipeline);
            }
            for (auto& built : writers) {  // roll back already-built pipelines
                TeardownWriter(*built);
            }
            return false;
        }

        auto writer = std::make_unique<CameraWriter>();
        writer->pipeline = pipeline;
        writer->appsrc = gst_bin_get_by_name(GST_BIN(pipeline), kProxyAppsrcName);
        if (writer->appsrc == nullptr ||
            gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            spdlog::error("RawCaptureSink::Start: cam{} appsrc/PLAYING failed", i);
            TeardownWriter(*writer);
            for (auto& built : writers) {
                TeardownWriter(*built);
            }
            return false;
        }
        writers.push_back(std::move(writer));
    }

    writers_ = std::move(writers);
    capture_group_id_ = capture_group_id;
    capturing_.store(true);
    spdlog::info("RawCaptureSink: started group={} cameras={} (vic={})", capture_group_id,
                 camera_count_, use_vic_);
    return true;
}

auto FilesystemRawCaptureSink::PushCamera(std::uint32_t camera_index,
                                          const sst::capture::Frame& frame) -> void {
    if (frame.planes.empty()) {
        return;
    }
    // try_to_lock keeps the non-blocking contract: if Start/Stop holds mtx_, drop
    // this frame rather than stall the shared fan-out thread. Holding the lock for
    // the appsrc push closes the race with Stop(), which moves writers_ out under
    // the same lock (no use-after-free on a runtime STOP mid-capture).
    std::unique_lock lock(mtx_, std::try_to_lock);
    if (!lock.owns_lock() || !capturing_.load() || camera_index >= writers_.size()) {
        return;
    }
    CameraWriter& writer = *writers_[camera_index];
    if (writer.appsrc == nullptr) {
        return;
    }
    if (!writer.caps_set) {
        GstCaps* caps = gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING, GstFormatFor(frame.format), "width", G_TYPE_INT,
            static_cast<int>(frame.geometry.width), "height", G_TYPE_INT,
            static_cast<int>(frame.geometry.height), "framerate", GST_TYPE_FRACTION,
            kProxySourceFramerateHint, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(writer.appsrc), caps);
        gst_caps_unref(caps);
        writer.caps_set = true;
    }

    const auto& plane = frame.planes[0];
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, plane.size, nullptr);
    if (buffer == nullptr) {
        return;
    }
    gst_buffer_fill(buffer, 0, plane.data, plane.size);
    // appsrc is block=false with a leaky=downstream queue behind it, so this
    // returns immediately and drops oldest under overload — never blocks capture.
    (void)gst_app_src_push_buffer(GST_APP_SRC(writer.appsrc), buffer);
}

auto FilesystemRawCaptureSink::Stop() -> bool {
    // Flip state + detach pipelines under the lock (O(1) move), then do the slow
    // EOS/moov-finalize work on the local copy OUTSIDE the lock so PushCamera
    // never contends with finalize. After the move, writers_ is empty and
    // capturing_ is false, so concurrent PushCamera calls no-op safely.
    std::vector<std::unique_ptr<CameraWriter>> draining;
    std::string group_id;
    {
        std::lock_guard lock(mtx_);
        if (!capturing_.load()) {
            return false;
        }
        capturing_.store(false);  // PushCamera no-ops from here
        draining = std::move(writers_);
        group_id = std::move(capture_group_id_);
        capture_group_id_.clear();
    }

    for (auto& writer : draining) {
        TeardownWriter(*writer);
    }
    spdlog::info("RawCaptureSink: stopped + finalized group={}", group_id);
    return true;
}

auto FilesystemRawCaptureSink::IsCapturing() const -> bool { return capturing_.load(); }

auto FilesystemRawCaptureSink::TeardownWriter(CameraWriter& writer) -> void {
    if (writer.appsrc != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(writer.appsrc));
    }
    // Wait for EOS (or error) so mp4mux writes the moov atom and the file plays.
    if (writer.pipeline != nullptr) {
        GstBus* bus = gst_element_get_bus(writer.pipeline);
        if (bus != nullptr) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, static_cast<GstClockTime>(kFinalizeTimeoutSeconds) * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (msg != nullptr) {
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        }
    }
    if (writer.appsrc != nullptr) {
        gst_object_unref(writer.appsrc);
        writer.appsrc = nullptr;
    }
    if (writer.pipeline != nullptr) {
        gst_element_set_state(writer.pipeline, GST_STATE_NULL);
        gst_object_unref(writer.pipeline);
        writer.pipeline = nullptr;
    }
}

}  // namespace sst::adapters::raw_capture
