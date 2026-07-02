#include "adapters/streaming/gst_rtmp/gst-rtmp-streamer.hpp"

#include <gst/app/gstappsrc.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "adapters/streaming/gst_rtmp/rtmp-launch.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/streaming/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::adapters::streaming {

namespace {

// How long the watcher blocks on the GStreamer bus per tick waiting for an
// uplink error before looping to re-check watching_.
constexpr int kBusPollMs = 200;
// Granularity of the backoff sleep, kept short so Stop() stays responsive.
constexpr int kBackoffSliceMs = 100;
// How long Stop() waits for EOS/error to flush the stream tail before NULLing
// the pipeline.
constexpr int kEosFlushSeconds = 5;

auto FrameByteSize(const sst::capture::Frame& frame) -> std::size_t {
    std::size_t total = 0;
    for (const auto& plane : frame.planes) {
        total += plane.size;
    }
    return total;
}

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
    return "BGR";
}

}  // namespace

GstRtmpStreamer::GstRtmpStreamer() { gst_init(nullptr, nullptr); }

GstRtmpStreamer::~GstRtmpStreamer() {
    if (running_) {
        Stop();
    }
}

auto GstRtmpStreamer::BuildAndPlayLocked() -> bool {
    // VIC offload ON by default; SST_DISABLE_VIC=1 forces software at runtime.
    const bool use_vic = std::getenv("SST_DISABLE_VIC") == nullptr;
    const std::string launch = BuildRtmpLaunch(config_, use_vic);
    GError* err = nullptr;
    pipeline_ = gst_parse_launch(launch.c_str(), &err);
    if (err != nullptr) {
        spdlog::error("GstRtmpStreamer: parse failed: {}", err->message);
        g_error_free(err);
        Teardown();
        return false;
    }
    if (pipeline_ == nullptr) {
        return false;
    }
    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), kRtmpAppsrcName);
    if (appsrc_ == nullptr) {
        spdlog::error("GstRtmpStreamer: appsrc not found");
        Teardown();
        return false;
    }
    // Fresh pipeline: input caps must be re-derived from the next frame (its real
    // geometry/format), which videoscale/videorate then conform to config_.
    caps_set_ = false;
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("GstRtmpStreamer: PLAYING transition failed");
        Teardown();
        return false;
    }
    return true;
}

auto GstRtmpStreamer::Start(const sst::streaming::PlatformStreamConfig& config) -> bool {
    std::lock_guard lock(mtx_);
    if (running_) {
        return true;
    }
    if (config.url.empty() || config.stream_key.empty()) {
        spdlog::error("GstRtmpStreamer::Start rejected: url/stream_key required ({})", config);
        return false;
    }
    config_ = config;
    spdlog::info("GstRtmpStreamer::Start {}", config_);

    if (!BuildAndPlayLocked()) {
        return false;
    }
    running_ = true;
    // Watch for uplink errors and rebuild the RTMP pipeline in place (rtmp2sink
    // has no auto-reconnect). Isolated to this branch — RTSP/capture are unaffected.
    watching_ = true;
    watcher_ = std::thread(&GstRtmpStreamer::WatcherLoop, this);
    return true;
}

auto GstRtmpStreamer::WatcherLoop() -> void {
    using std::chrono::milliseconds;
    // Capped exponential backoff so a permanently-down endpoint can't spin into a
    // reconnect storm (was an unbounded ~5 rebuilds/sec). Reset to base on a
    // healthy tick or a successful reconnect.
    constexpr auto kBaseBackoff = milliseconds(500);
    constexpr auto kMaxBackoff = milliseconds(8000);
    auto backoff = kBaseBackoff;

    while (watching_.load()) {
        GstBus* bus = nullptr;
        {
            std::lock_guard lock(mtx_);
            if (pipeline_ != nullptr) {
                bus = gst_element_get_bus(pipeline_);
            }
        }

        if (bus != nullptr) {
            GstMessage* msg =
                gst_bus_timed_pop_filtered(bus, kBusPollMs * GST_MSECOND, GST_MESSAGE_ERROR);
            gst_object_unref(bus);
            if (msg == nullptr) {
                backoff = kBaseBackoff;  // healthy tick — uplink is alive
                continue;
            }
            gst_message_unref(msg);
            spdlog::warn("GstRtmpStreamer: RTMP bus error — rebuilding uplink");
        }
        // bus == nullptr means a prior rebuild failed (pipeline torn down). There
        // is no bus to surface a future error, so retry the rebuild ourselves
        // rather than spin idle — otherwise the uplink would stay dead while
        // IsRunning() still reported true.

        // Back off before (re)building. Sleep in short slices so Stop() — which
        // clears watching_ then joins — stays responsive.
        for (auto waited = milliseconds(0); watching_.load() && waited < backoff;
             waited += milliseconds(kBackoffSliceMs)) {
            std::this_thread::sleep_for(milliseconds(kBackoffSliceMs));
        }

        std::lock_guard lock(mtx_);
        if (!watching_.load() || !running_.load()) {
            break;
        }
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        Teardown();
        if (BuildAndPlayLocked()) {
            backoff = kBaseBackoff;
            spdlog::info("GstRtmpStreamer: uplink reconnected");
        } else {
            backoff = std::min(backoff * 2, kMaxBackoff);
            spdlog::error("GstRtmpStreamer: reconnect failed; retrying in {} ms", backoff.count());
        }
    }
}

auto GstRtmpStreamer::Stop() -> void {
    // Stop the watcher first (outside mtx_) so its reconnect path can't race the
    // teardown below.
    watching_ = false;
    if (watcher_.joinable()) {
        watcher_.join();
    }

    std::lock_guard lock(mtx_);
    if (!running_) {
        return;
    }
    if (appsrc_ != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    }
    if (pipeline_ != nullptr) {
        // Wait for EOS (or error) so flvmux/rtmpsink flush the tail of the
        // stream before we cut the pipeline — otherwise the last GOP is lost.
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus != nullptr) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, kEosFlushSeconds * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (msg != nullptr) {
                gst_message_unref(msg);
            }
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    Teardown();
    running_ = false;
    spdlog::info("GstRtmpStreamer::Stop id={}", config_.stream_id);
}

auto GstRtmpStreamer::IsRunning() const -> bool { return running_; }
auto GstRtmpStreamer::Id() const -> std::int64_t { return config_.stream_id; }

auto GstRtmpStreamer::Teardown() -> void {
    if (appsrc_ != nullptr) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

auto GstRtmpStreamer::Push(const sst::capture::Frame& frame) -> void {
    GstElement* src = nullptr;
    {
        // try_to_lock: never block the shared fan-out thread. A reconnect holds
        // mtx_ across a blocking GST_STATE_NULL teardown + rebuild; without this
        // a stalled RTMP uplink would stall StreamingService::Push and degrade
        // the otherwise-independent RTSP preview. Dropping RTMP frames while the
        // uplink is down is correct — there is nowhere to send them.
        std::unique_lock lock(mtx_, std::try_to_lock);
        if (!lock.owns_lock() || !running_ || appsrc_ == nullptr) {
            return;
        }
        if (!caps_set_) {
            // Input caps describe the SOURCE frame (postprocess output) — its real
            // pixel geometry/format. The launch string's videoscale/videorate then
            // conform it to config_'s requested stream resolution/fps.
            GstCaps* caps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, GstFormatFor(frame.format), "width",
                G_TYPE_INT, static_cast<int>(frame.geometry.width), "height", G_TYPE_INT,
                static_cast<int>(frame.geometry.height), "framerate", GST_TYPE_FRACTION,
                config_.framerate, 1, nullptr);
            gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
            gst_caps_unref(caps);
            caps_set_ = true;
        }
        src = appsrc_;
        gst_object_ref(src);
    }

    const auto total = FrameByteSize(frame);
    if (total == 0) {
        gst_object_unref(src);
        return;
    }

    auto* gst_buf = gst_buffer_new_allocate(nullptr, total, nullptr);
    if (gst_buf == nullptr) {
        gst_object_unref(src);
        return;
    }
    GstMapInfo map{};
    if (gst_buffer_map(gst_buf, &map, GST_MAP_WRITE) == 0) {
        gst_buffer_unref(gst_buf);
        gst_object_unref(src);
        return;
    }
    std::size_t offset = 0;
    for (const auto& plane : frame.planes) {
        if (plane.data != nullptr && plane.size > 0) {
            std::memcpy(map.data + offset, plane.data, plane.size);
            offset += plane.size;
        }
    }
    gst_buffer_unmap(gst_buf, &map);

    // Leave timestamps unset so the do-timestamp=true appsrc stamps a valid
    // running-time PTS. Forcing GST_CLOCK_TIME_NONE corrupts the x264enc
    // software-encoded stream (nvv4l2h264enc tolerated it; x264enc does not).

    (void)gst_app_src_push_buffer(GST_APP_SRC(src), gst_buf);
    gst_object_unref(src);
}

}  // namespace sst::adapters::streaming
