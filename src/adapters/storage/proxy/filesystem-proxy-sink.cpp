#include "adapters/storage/proxy/filesystem-proxy-sink.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <system_error>
#include <utility>

#include "adapters/common/gstreamer/gst-frame.hpp"
#include "adapters/storage/proxy/proxy-launch.hpp"
#include "domain/storage/models/proxy-identity.hpp"
#include "domain/storage/services/proxy-naming.hpp"

namespace sst::adapters::storage {

namespace {
// Bound the wait for mp4mux to flush its moov on Stop so a stuck proxy pipeline
// can't hang shutdown; the leaky pre-encoder queue caps how large the residual
// backlog can get. Matches the record recorder's finalize budget.
constexpr int kFinalizeTimeoutSeconds = 10;

// Nominal source framerate for the appsrc input caps. do-timestamp=true supplies
// the real PTS and the pipeline's videorate conforms to the proxy fps, so this
// is only a caps hint — the true capture cadence varies (30/60).
constexpr int kProxySourceFramerateHint = 30;

// Shared helper (adapters/common/gstreamer); "NV12" is this adapter's
// historical fallback for an out-of-enum format (proxy input is NV12).
auto GstFormatFor(sst::common::PixelFormat fmt) -> const char* {
    return sst::adapters::gst_common::GstFormatFor(fmt, "NV12");
}
}  // namespace

FilesystemProxySink::FilesystemProxySink(std::filesystem::path video_dir,
                                         std::uint32_t camera_count)
    : video_dir_(std::move(video_dir)),
      camera_count_(camera_count),
      // VIC offload on by default; SST_DISABLE_VIC=1 forces software scale/convert
      // (matches the record/RTMP paths so one switch toggles the whole box).
      use_vic_(std::getenv("SST_DISABLE_VIC") == nullptr) {
    gst_init(nullptr, nullptr);
}

FilesystemProxySink::~FilesystemProxySink() {
    if (capturing_.load()) {
        (void)Stop();
    }
}

// floor-ok: linear per-camera GStreamer proxy setup + validation guards.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto FilesystemProxySink::Start(const std::string& match_uuid,
                                const std::filesystem::path& output_dir) -> bool {
    std::lock_guard lock(mtx_);
    if (capturing_.load()) {
        spdlog::warn("ProxySink::Start: already capturing");
        return false;
    }
    if (match_uuid.empty()) {
        spdlog::error("ProxySink::Start: empty match_uuid");
        return false;
    }

    // Write the proxy pair INTO the per-match dir (SessionConfig.video_output_path)
    // so it sits beside the final <match>.mp4 + timeline.json. Empty => fall back
    // to the construction-time root.
    const std::filesystem::path dir = output_dir.empty() ? video_dir_ : output_dir;

    std::error_code err;
    std::filesystem::create_directories(dir, err);
    if (err) {
        spdlog::error("ProxySink::Start: cannot create {}: {}", dir.string(), err.message());
        return false;
    }

    // A re-run for the same match (stop + start within one match session)
    // truncate-overwrites the previous proxy pair via filesink, mirroring the L1
    // recorder writing the same <match>.mp4 — the proxy always mirrors its
    // match's latest take.
    std::vector<std::unique_ptr<CameraWriter>> writers;
    writers.reserve(camera_count_);
    for (std::uint32_t i = 0; i < camera_count_; ++i) {
        const auto name = sst::storage::proxy_naming::FileName(
            sst::storage::ProxyIdentity{.match_uuid = match_uuid, .camera_index = i});
        const auto path = dir / name;

        const std::string desc = BuildProxyLaunch(path.string(), use_vic_);
        GError* gerr = nullptr;
        GstElement* pipeline = gst_parse_launch(desc.c_str(), &gerr);
        if (pipeline == nullptr || gerr != nullptr) {
            spdlog::error("ProxySink::Start: parse failed for cam{}: {}", i,
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
            spdlog::error("ProxySink::Start: cam{} appsrc/PLAYING failed", i);
            TeardownWriter(*writer);
            for (auto& built : writers) {
                TeardownWriter(*built);
            }
            return false;
        }
        writers.push_back(std::move(writer));
    }

    writers_ = std::move(writers);
    match_uuid_ = match_uuid;
    capturing_.store(true);
    spdlog::info("ProxySink: started match={} cameras={} (vic={})", match_uuid, camera_count_,
                 use_vic_);
    return true;
}

auto FilesystemProxySink::PushCamera(std::uint32_t camera_index,
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

    // Copy EVERY plane (shared helper): real NV12 frames are 2-plane (Y + UV) —
    // pushing only planes[0] drops the chroma plane and writes color-broken
    // proxy MP4s on device.
    GstBuffer* buffer = sst::adapters::gst_common::MakeGstBufferFromFrame(frame);
    if (buffer == nullptr) {
        return;
    }
    // appsrc is block=false with a leaky=downstream queue behind it, so this
    // returns immediately and drops oldest under overload — never blocks capture.
    (void)gst_app_src_push_buffer(GST_APP_SRC(writer.appsrc), buffer);
}

auto FilesystemProxySink::Stop() -> bool {
    // Flip state + detach pipelines under the lock (O(1) move), then do the slow
    // EOS/moov-finalize work on the local copy OUTSIDE the lock so PushCamera
    // never contends with finalize. After the move, writers_ is empty and
    // capturing_ is false, so concurrent PushCamera calls no-op safely.
    std::vector<std::unique_ptr<CameraWriter>> draining;
    std::string match_uuid;
    {
        std::lock_guard lock(mtx_);
        if (!capturing_.load()) {
            return false;
        }
        capturing_.store(false);  // PushCamera no-ops from here
        draining = std::move(writers_);
        match_uuid = std::move(match_uuid_);
        match_uuid_.clear();
    }

    for (auto& writer : draining) {
        TeardownWriter(*writer);
    }
    spdlog::info("ProxySink: stopped + finalized match={}", match_uuid);
    return true;
}

auto FilesystemProxySink::IsCapturing() const -> bool { return capturing_.load(); }

auto FilesystemProxySink::TeardownWriter(CameraWriter& writer) -> void {
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

}  // namespace sst::adapters::storage
